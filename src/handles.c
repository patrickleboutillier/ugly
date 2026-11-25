/*
 * The handle table: every handle is a slot index plus a generation, never a
 * pointer. Owned slots hold the native ref and a reference count; borrow
 * slots point at an owned slot and are revoked when their call returns.
 * Owned slots with an identity key are indexed by (home, identity) so the
 * same native object maps to the same handle.
 *
 * One global lock protects the table, the identity index and every
 * runtime's queue of deferred releases. Plugin code never runs under it.
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"


enum { SLOT_FREE = 0, SLOT_OWNED, SLOT_BORROW } ;

typedef struct slot {
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

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER ;
static slot *slots ;
static uint32_t nslots, slots_cap ;
static uint32_t free_head ;

static uint32_t *buckets ;   /* identity index: heads, index + 1 */
static uint32_t nbuckets ;   /* power of two, or 0 */
static uint32_t nidentities ;

#define NO_SLOT UINT32_MAX


/* ---------------------------------------------------------------- */
/* Slots                                                            */
/* ---------------------------------------------------------------- */

static ugly_handle encode(uint32_t i){
	return ((uint64_t)slots[i].gen << 32) | (uint64_t)(i + 1) ;
}


static int decode(ugly_handle h, uint32_t *i){
	uint64_t lo = h & 0xffffffffu ;
	if ((lo == 0)||(lo > nslots)){
		return 0 ;
	}
	uint32_t idx = (uint32_t)(lo - 1) ;
	if ((slots[idx].kind == SLOT_FREE)||(slots[idx].gen != (uint32_t)(h >> 32))){
		return 0 ;
	}
	*i = idx ;
	return 1 ;
}


/* The owned slot a handle stands for (itself, or a borrow's target). */
static int decode_owned(ugly_handle h, uint32_t *i){
	uint32_t idx ;
	if (! decode(h, &idx)){
		return 0 ;
	}
	*i = (slots[idx].kind == SLOT_BORROW ? slots[idx].target : idx) ;
	return 1 ;
}


static uint32_t alloc_slot(void){
	uint32_t i ;
	if (free_head != 0){
		i = free_head - 1 ;
		free_head = slots[i].free_next ;
	}
	else {
		if (nslots == UINT32_MAX - 1){
			return NO_SLOT ;
		}
		if (nslots == slots_cap){
			uint32_t ncap = (slots_cap ? slots_cap * 2 : 64) ;
			slot *ns = realloc(slots, ncap * sizeof(slot)) ;
			if (ns == NULL){
				return NO_SLOT ;
			}
			slots = ns ;
			slots_cap = ncap ;
		}
		i = nslots++ ;
		slots[i].gen = 1 ;
	}
	uint32_t gen = slots[i].gen ;
	memset(&slots[i], 0, sizeof(slot)) ;
	slots[i].gen = gen ;
	return i ;
}


static void free_slot(uint32_t i){
	slots[i].kind = SLOT_FREE ;
	slots[i].ref = NULL ;
	slots[i].home = NULL ;
	slots[i].gen++ ;
	if (slots[i].gen == 0){
		slots[i].gen = 1 ;
	}
	slots[i].free_next = free_head ;
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
		slot *s = &slots[n - 1] ;
		if ((s->home == home)&&(s->identity == identity)){
			return n - 1 ;
		}
		n = s->id_next ;
	}
	return NO_SLOT ;
}


static void id_link(uint32_t i){
	uint32_t b = id_hash(slots[i].home, slots[i].identity) & (nbuckets - 1) ;
	slots[i].id_next = buckets[b] ;
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
			if ((slots[j].kind == SLOT_OWNED)&&(slots[j].identity != 0)&&(j != i)){
				id_link(j) ;
			}
		}
	}
	id_link(i) ;
	nidentities++ ;
	return 1 ;
}


static void id_remove(uint32_t i){
	if (slots[i].identity == 0){
		return ;
	}
	uint32_t *p = &buckets[id_hash(slots[i].home, slots[i].identity) & (nbuckets - 1)] ;
	while (*p != 0){
		if (*p == i + 1){
			*p = slots[i].id_next ;
			nidentities-- ;
			return ;
		}
		p = &slots[*p - 1].id_next ;
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
	rt->pending[rt->npending++] = ref ;
	return 1 ;
}


/*
 * Drops one reference on owned slot i (lock held). If it was the last one,
 * frees the slot and either returns the native ref for the caller to release
 * (on the home thread, after unlocking) or queues it for the home runtime.
 */
static int unref_locked(uint32_t i, ugly_rt **home, void **ref){
	if (--slots[i].refs > 0){
		return 0 ;
	}
	ugly_rt *h = slots[i].home ;
	void *r = slots[i].ref ;
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
			slots[f].refs++ ;
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
	slots[i].kind = SLOT_OWNED ;
	slots[i].home = rt ;
	slots[i].ref = ref ;
	slots[i].identity = identity ;
	slots[i].refs = 1 ;
	if ((identity != 0)&&(! id_insert(i))){
		free_slot(i) ;
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_NOMEM ;
	}
	*out = encode(i) ;
	pthread_mutex_unlock(&lock) ;
	return UGLY_OK ;
}


ugly_status h_lookup(ugly_handle h, ugly_rt **home, void **ref){
	pthread_mutex_lock(&lock) ;
	uint32_t i ;
	if (! decode_owned(h, &i)){
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_STALE_HANDLE ;
	}
	*home = slots[i].home ;
	*ref = slots[i].ref ;
	pthread_mutex_unlock(&lock) ;
	return UGLY_OK ;
}


ugly_status h_dup(ugly_handle h, ugly_handle *out){
	pthread_mutex_lock(&lock) ;
	uint32_t i ;
	if (! decode_owned(h, &i)){
		pthread_mutex_unlock(&lock) ;
		return UGLY_ERR_STALE_HANDLE ;
	}
	slots[i].refs++ ;
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
	if (slots[i].kind == SLOT_BORROW){
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
	slots[b].kind = SLOT_BORROW ;
	slots[b].target = t ;
	slots[t].refs++ ;          /* the target stays alive for the call, whatever the caller does */
	*out = encode(b) ;
	pthread_mutex_unlock(&lock) ;
	return UGLY_OK ;
}


void h_revoke(ugly_handle borrow){
	pthread_mutex_lock(&lock) ;
	uint32_t b ;
	if ((! decode(borrow, &b))||(slots[b].kind != SLOT_BORROW)){
		/* Already gone: its target's runtime was deleted during the call. */
		pthread_mutex_unlock(&lock) ;
		return ;
	}
	uint32_t t = slots[b].target ;
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
		if ((slots[i].kind == SLOT_OWNED)&&(slots[i].home == rt)){
			if (*nrefs == cap){
				cap = (cap ? cap * 2 : 16) ;
				void **nr = realloc(*refs, cap * sizeof(void *)) ;
				if (nr == NULL){
					break ;
				}
				*refs = nr ;
			}
			(*refs)[(*nrefs)++] = slots[i].ref ;
			id_remove(i) ;
			free_slot(i) ;
		}
	}
	/* Borrows of those values (in calls still running elsewhere) die with them. */
	for (i = 0 ; i < nslots ; i++){
		if ((slots[i].kind == SLOT_BORROW)&&(slots[slots[i].target].kind == SLOT_FREE)){
			free_slot(i) ;
		}
	}
	pthread_mutex_unlock(&lock) ;
}


void h_drain(ugly_rt *rt){
	pthread_mutex_lock(&lock) ;
	void **pending = rt->pending ;
	size_t n = rt->npending ;
	rt->pending = NULL ;
	rt->npending = rt->pending_cap = 0 ;
	pthread_mutex_unlock(&lock) ;

	size_t i ;
	for (i = 0 ; i < n ; i++){
		native_release(rt, pending[i]) ;
	}
	free(pending) ;
}
