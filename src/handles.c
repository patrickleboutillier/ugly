/*
 * The handle table: every handle is a slot index plus a generation, never a
 * pointer. Owned slots hold the native ref and a reference count; borrow
 * slots point at an owned slot and are revoked when their call returns.
 * Owned slots with an identity key are indexed by (home, identity) so the
 * same native object maps to the same handle.
 *
 * One global lock protects the table, the identity index and every
 * runtime's queue of deferred releases. Plugin code never runs under it.
 *
 * h_lookup, which every call makes, doesn't take it: with one lock, threads
 * calling into their own runtimes slowed each other down (tests/bench.c).
 * Slots live in chunks that never move, and a slot's fields that a lookup
 * reads (kind, gen, target, home, ref) are written under the lock, inside a
 * per-slot sequence: odd while they change. A lookup reads the sequence, the
 * fields, then the sequence again, and retries if it moved (a seqlock).
 * Reference counts and the other fields are the lock's alone.
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"


enum { SLOT_FREE = 0, SLOT_OWNED, SLOT_BORROW } ;

typedef struct slot {
	uint32_t seq ;           /* odd while kind, gen, target, home or ref change */
	uint32_t gen ;
	uint8_t kind ;
	uint32_t free_next ;     /* FREE: next free slot, index + 1 (0: none) */
	uint32_t id_next ;       /* OWNED with identity: next in its bucket, index + 1 */
	ugly_rt *home ;
	void *ref ;
	uintptr_t identity ;
	uint64_t refs ;
	uint32_t target ;        /* BORROW: the owned slot's index */
} slot ;

/* Slots in chunks that never move or go away, so a lookup can read one without the lock. */
#define CHUNK_BITS 12
#define CHUNK (1u << CHUNK_BITS)
#define MAX_CHUNKS (1u << 16)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER ;
static slot *chunks[MAX_CHUNKS] ;
static uint32_t nslots, slots_cap ;     /* nslots: stored with release, for h_lookup */
static uint32_t free_head ;

static uint32_t *buckets ;   /* identity index: heads, index + 1 */
static uint32_t nbuckets ;   /* power of two, or 0 */
static uint32_t nidentities ;

#define NO_SLOT UINT32_MAX

/* Slot i (lock held, or i < an nslots read with acquire). */
#define S(i) (&chunks[(i) >> CHUNK_BITS][(i) & (CHUNK - 1)])

/* The fields h_lookup reads are written atomically, inside write_begin/write_end. */
#define PUT(field, v) __atomic_store_n(&(field), (v), __ATOMIC_RELAXED)
#define GET(field) __atomic_load_n(&(field), __ATOMIC_RELAXED)

static void write_begin(slot *s){
	__atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELAXED) ;
	__atomic_thread_fence(__ATOMIC_RELEASE) ;
}

static void write_end(slot *s){
	__atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE) ;
}


/* ---------------------------------------------------------------- */
/* Slots                                                            */
/* ---------------------------------------------------------------- */

static ugly_handle encode(uint32_t i){
	return ((uint64_t)S(i)->gen << 32) | (uint64_t)(i + 1) ;
}


/* Lock held. */
static int decode(ugly_handle h, uint32_t *i){
	uint64_t lo = h & 0xffffffffu ;
	if ((lo == 0)||(lo > nslots)){
		return 0 ;
	}
	uint32_t idx = (uint32_t)(lo - 1) ;
	if ((S(idx)->kind == SLOT_FREE)||(S(idx)->gen != (uint32_t)(h >> 32))){
		return 0 ;
	}
	*i = idx ;
	return 1 ;
}


/* The owned slot a handle stands for (itself, or a borrow's target). Lock held. */
static int decode_owned(ugly_handle h, uint32_t *i){
	uint32_t idx ;
	if (! decode(h, &idx)){
		return 0 ;
	}
	*i = (S(idx)->kind == SLOT_BORROW ? S(idx)->target : idx) ;
	return 1 ;
}


/* A free slot, its fields cleared (still SLOT_FREE: the caller fills it in). */
static uint32_t alloc_slot(void){
	uint32_t i ;
	if (free_head != 0){
		i = free_head - 1 ;
		free_head = S(i)->free_next ;
	}
	else {
		if (nslots == MAX_CHUNKS * CHUNK){
			return NO_SLOT ;
		}
		if (nslots == slots_cap){
			slot *c = calloc(CHUNK, sizeof(slot)) ;
			if (c == NULL){
				return NO_SLOT ;
			}
			__atomic_store_n(&chunks[slots_cap >> CHUNK_BITS], c, __ATOMIC_RELEASE) ;
			slots_cap += CHUNK ;
		}
		i = nslots ;
		PUT(S(i)->gen, 1) ;
		__atomic_store_n(&nslots, nslots + 1, __ATOMIC_RELEASE) ;
	}
	slot *s = S(i) ;
	write_begin(s) ;
	PUT(s->target, 0) ;
	PUT(s->home, NULL) ;
	PUT(s->ref, NULL) ;
	write_end(s) ;
	s->free_next = 0 ;
	s->id_next = 0 ;
	s->identity = 0 ;
	s->refs = 0 ;
	return i ;
}


static void free_slot(uint32_t i){
	slot *s = S(i) ;
	uint32_t gen = s->gen + 1 ;
	write_begin(s) ;
	PUT(s->kind, SLOT_FREE) ;
	PUT(s->ref, NULL) ;
	PUT(s->home, NULL) ;
	PUT(s->gen, gen == 0 ? 1 : gen) ;
	write_end(s) ;
	s->free_next = free_head ;
	free_head = i + 1 ;
}


/* ---------------------------------------------------------------- */
/* Identity index                                                   */
/* ---------------------------------------------------------------- */

static uint32_t id_hash(const ugly_rt *home, uintptr_t identity){
	uint64_t x = (uint64_t)identity ^ ((uint64_t)(uintptr_t)home * 0x9E3779B97F4A7C15ull) ;
	x ^= x >> 31 ;
	x *= 0xBF58476D1CE4E5B9ull ;
	x ^= x >> 29 ;
	return (uint32_t)x ;
}


static uint32_t id_find(const ugly_rt *home, uintptr_t identity){
	if (nbuckets == 0){
		return NO_SLOT ;
	}
	uint32_t n = buckets[id_hash(home, identity) & (nbuckets - 1)] ;
	while (n != 0){
		slot *s = S(n - 1) ;
		if ((s->home == home)&&(s->identity == identity)){
			return n - 1 ;
		}
		n = s->id_next ;
	}
	return NO_SLOT ;
}


static void id_link(uint32_t i){
	uint32_t b = id_hash(S(i)->home, S(i)->identity) & (nbuckets - 1) ;
	S(i)->id_next = buckets[b] ;
	buckets[b] = i + 1 ;
}


static int id_insert(uint32_t i){
	if (nidentities + 1 > nbuckets){
		uint32_t nb = (nbuckets ? nbuckets * 2 : 64) ;
		uint32_t *nbk = calloc(nb, sizeof(uint32_t)) ;
		if (nbk == NULL){
			return 0 ;
		}
		free(buckets) ;
		buckets = nbk ;
		nbuckets = nb ;
		uint32_t j ;
		for (j = 0 ; j < nslots ; j++){
			if ((S(j)->kind == SLOT_OWNED)&&(S(j)->identity != 0)&&(j != i)){
				id_link(j) ;
			}
		}
	}
	id_link(i) ;
	nidentities++ ;
	return 1 ;
}


static void id_remove(uint32_t i){
	if (S(i)->identity == 0){
		return ;
	}
	uint32_t *p = &buckets[id_hash(S(i)->home, S(i)->identity) & (nbuckets - 1)] ;
	while (*p != 0){
		if (*p == i + 1){
			*p = S(i)->id_next ;
			nidentities-- ;
			return ;
		}
		p = &S(*p - 1)->id_next ;
	}
}


/* ---------------------------------------------------------------- */
/* Releasing                                                        */
/* ---------------------------------------------------------------- */

static int queue_pending(ugly_rt *rt, void *ref){
	if (rt->npending == rt->pending_cap){
		size_t ncap = (rt->pending_cap ? rt->pending_cap * 2 : 16) ;
		void **np = realloc(rt->pending, ncap * sizeof(void *)) ;
		if (np == NULL){
			return 0 ;
		}
		rt->pending = np ;
		rt->pending_cap = ncap ;
	}
	rt->pending[rt->npending] = ref ;
	__atomic_store_n(&rt->npending, rt->npending + 1, __ATOMIC_RELEASE) ;     /* h_drain looks without the lock */
	return 1 ;
}


/*
 * Drops one reference on owned slot i (lock held). If it was the last one,
 * frees the slot and either returns the native ref for the caller to release
 * (on the home thread, after unlocking) or queues it for the home runtime.
 */
static int unref_locked(uint32_t i, ugly_rt **home, void **ref){
	if (--S(i)->refs > 0){
		return 0 ;
	}
	ugly_rt *h = S(i)->home ;
	void *r = S(i)->ref ;
	id_remove(i) ;
	free_slot(i) ;
	/*
	 * Released here, on its home thread; otherwise queued for the next entry
	 * into the home runtime. A runtime any thread may enter is always
	 * queued: released here, it could race with its deletion on another
	 * thread, while an entry holds it.
	 */
	if (on_thread(h->thread)&&(! rt_any_thread(h))){
		*home = h ;
		*ref = r ;
		return 1 ;
	}
	queue_pending(h, r) ;    /* if this fails, the native ref leaks rather than crashing */
	return 0 ;
}


static void native_release(ugly_rt *home, void *ref){
	home->lang->plugin->release(home, ref) ;
}


/* ---------------------------------------------------------------- */
/* Internal API                                                     */
/* ---------------------------------------------------------------- */

ugly_status h_export(ugly_rt *rt, void *ref, uintptr_t identity, ugly_handle *out, int *duplicate){
	*duplicate = 0 ;
	pthread_mutex_lock(&lock) ;
	if (rt->dying){
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_STALE_HANDLE ;
	}
	if (identity != 0){
		uint32_t f = id_find(rt, identity) ;
		if (f != NO_SLOT){
			S(f)->refs++ ;
			*out = encode(f) ;
			*duplicate = 1 ;
			pthread_mutex_unlock(&lock) ;
			return UGLY_OK ;
		}
	}
	uint32_t i = alloc_slot() ;
	if (i == NO_SLOT){
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_NOMEM ;
	}
	slot *s = S(i) ;
	write_begin(s) ;
	PUT(s->kind, SLOT_OWNED) ;
	PUT(s->home, rt) ;
	PUT(s->ref, ref) ;
	write_end(s) ;
	s->identity = identity ;
	s->refs = 1 ;
	if ((identity != 0)&&(! id_insert(i))){
		free_slot(i) ;
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_NOMEM ;
	}
	*out = encode(i) ;
	pthread_mutex_unlock(&lock) ;
	return UGLY_OK ;
}


/* Slot i, for a reader without the lock (i < an nslots read with acquire). */
static slot *slot_acquire(uint32_t i){
	slot *c = __atomic_load_n(&chunks[i >> CHUNK_BITS], __ATOMIC_ACQUIRE) ;
	return &c[i & (CHUNK - 1)] ;
}


static ugly_status lookup_locked(ugly_handle h, ugly_rt **home, void **ref){
	pthread_mutex_lock(&lock) ;
	uint32_t i ;
	if (! decode_owned(h, &i)){
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_STALE_HANDLE ;
	}
	*home = S(i)->home ;
	*ref = S(i)->ref ;
	pthread_mutex_unlock(&lock) ;
	return UGLY_OK ;
}


/*
 * Every call starts here, so no lock (see the top of this file): a seqlock
 * read of the handle's slot, and of its target for a borrow. A writer in
 * the middle of a change makes us try again, a few times, then take the
 * lock like everyone else.
 */
ugly_status h_lookup(ugly_handle h, ugly_rt **home, void **ref){
	uint64_t lo = h & 0xffffffffu ;
	if ((lo == 0)||(lo > __atomic_load_n(&nslots, __ATOMIC_ACQUIRE))){
		return UGLY_ERR_STALE_HANDLE ;
	}
	slot *s = slot_acquire((uint32_t)(lo - 1)) ;
	int tries ;
	for (tries = 0 ; tries < 64 ; tries++){
		uint32_t q = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE), qt = 0 ;
		if (q & 1){
			continue ;
		}
		uint8_t kind = GET(s->kind) ;
		int live = (kind != SLOT_FREE)&&(GET(s->gen) == (uint32_t)(h >> 32)) ;
		slot *t = s ;
		if (live && (kind == SLOT_BORROW)){
			t = slot_acquire(GET(s->target)) ;
			qt = __atomic_load_n(&t->seq, __ATOMIC_ACQUIRE) ;
			if (qt & 1){
				continue ;
			}
			live = (GET(t->kind) == SLOT_OWNED) ;
		}
		ugly_rt *hm = (live ? GET(t->home) : NULL) ;
		void *r = (live ? GET(t->ref) : NULL) ;
		__atomic_thread_fence(__ATOMIC_ACQUIRE) ;
		if ((__atomic_load_n(&s->seq, __ATOMIC_RELAXED) != q)
			||((t != s)&&(__atomic_load_n(&t->seq, __ATOMIC_RELAXED) != qt))){
			continue ;
		}
		if (! live){
			return UGLY_ERR_STALE_HANDLE ;
		}
		*home = hm ;
		*ref = r ;
		return UGLY_OK ;
	}
	return lookup_locked(h, home, ref) ;
}


ugly_status h_dup(ugly_handle h, ugly_handle *out){
	pthread_mutex_lock(&lock) ;
	uint32_t i ;
	if (! decode_owned(h, &i)){
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_STALE_HANDLE ;
	}
	S(i)->refs++ ;
	*out = encode(i) ;
	pthread_mutex_unlock(&lock) ;
	return UGLY_OK ;
}


ugly_status h_release(ugly_handle h){
	pthread_mutex_lock(&lock) ;
	uint32_t i ;
	if (! decode(h, &i)){
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_STALE_HANDLE ;
	}
	if (S(i)->kind == SLOT_BORROW){
		/* A borrow isn't the caller's to release: it ends with its call. */
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_USAGE ;
	}
	ugly_rt *home = NULL ;
	void *ref = NULL ;
	int now = unref_locked(i, &home, &ref) ;
	pthread_mutex_unlock(&lock) ;
	if (now){
		native_release(home, ref) ;
	}
	return UGLY_OK ;
}


ugly_status h_borrow(ugly_handle h, ugly_handle *out){
	pthread_mutex_lock(&lock) ;
	uint32_t t ;
	if (! decode_owned(h, &t)){
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_STALE_HANDLE ;
	}
	uint32_t b = alloc_slot() ;
	if (b == NO_SLOT){
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_NOMEM ;
	}
	slot *bs = S(b) ;
	write_begin(bs) ;
	PUT(bs->kind, SLOT_BORROW) ;
	PUT(bs->target, t) ;
	write_end(bs) ;
	S(t)->refs++ ;          /* the target stays alive for the call, whatever the caller does */
	*out = encode(b) ;
	pthread_mutex_unlock(&lock) ;
	return UGLY_OK ;
}


void h_revoke(ugly_handle borrow){
	pthread_mutex_lock(&lock) ;
	uint32_t b ;
	if ((! decode(borrow, &b))||(S(b)->kind != SLOT_BORROW)){
		/* Already gone: its target's runtime was deleted during the call. */
		pthread_mutex_unlock(&lock) ;
		return ;
	}
	uint32_t t = S(b)->target ;
	free_slot(b) ;
	ugly_rt *home = NULL ;
	void *ref = NULL ;
	int now = unref_locked(t, &home, &ref) ;
	pthread_mutex_unlock(&lock) ;
	if (now){
		native_release(home, ref) ;
	}
}


ugly_status h_same(ugly_handle a, ugly_handle b, int *out){
	pthread_mutex_lock(&lock) ;
	uint32_t ia, ib ;
	if ((! decode_owned(a, &ia))||(! decode_owned(b, &ib))){
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_STALE_HANDLE ;
	}
	*out = (ia == ib) ;
	pthread_mutex_unlock(&lock) ;
	return UGLY_OK ;
}


void h_invalidate(ugly_rt *rt, void ***refs, size_t *nrefs){
	*refs = NULL ;
	*nrefs = 0 ;
	size_t cap = 0 ;

	pthread_mutex_lock(&lock) ;
	uint32_t i ;
	for (i = 0 ; i < nslots ; i++){
		if ((S(i)->kind == SLOT_OWNED)&&(S(i)->home == rt)){
			if (*nrefs == cap){
				cap = (cap ? cap * 2 : 16) ;
				void **nr = realloc(*refs, cap * sizeof(void *)) ;
				if (nr == NULL){
					break ;
				}
				*refs = nr ;
			}
			(*refs)[(*nrefs)++] = S(i)->ref ;
			id_remove(i) ;
			free_slot(i) ;
		}
	}
	/* Borrows of those values (in calls still running elsewhere) die with them. */
	for (i = 0 ; i < nslots ; i++){
		if ((S(i)->kind == SLOT_BORROW)&&(S(S(i)->target)->kind == SLOT_FREE)){
			free_slot(i) ;
		}
	}
	pthread_mutex_unlock(&lock) ;
}


void h_drain(ugly_rt *rt){
	/*
	 * Every call into a runtime comes here, and the queue is nearly always
	 * empty: look first, without the lock. A release queued just after we
	 * looked is drained at the next entry, as one queued after the drain
	 * would be.
	 */
	if (__atomic_load_n(&rt->npending, __ATOMIC_ACQUIRE) == 0){
		return ;
	}
	pthread_mutex_lock(&lock) ;
	void **pending = rt->pending ;
	size_t n = rt->npending ;
	rt->pending = NULL ;
	__atomic_store_n(&rt->npending, 0, __ATOMIC_RELAXED) ;
	rt->pending_cap = 0 ;
	pthread_mutex_unlock(&lock) ;

	size_t i ;
	for (i = 0 ; i < n ; i++){
		native_release(rt, pending[i]) ;
	}
	free(pending) ;
}
