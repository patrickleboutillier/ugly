/*
 * Threads: ctxs belong to one thread, runtimes too unless their plugin has
 * UGLY_CAP_ANY_THREAD, and releases from other threads are deferred (§6, §3).
 */
#include <pthread.h>
#include <semaphore.h>
#include "conformance.h"
#include "ugly_plugin.h"     /* ugly_rt_set_host_serialized, ugly_rt_drain */


typedef struct {
	fixture *f ;
	ugly_handle counter ;        /* owned by the main thread's runtime; this thread releases it */
	ugly_handle callable ;       /* a C callable homed on the main thread; released here too */
	ugly_status call_st, ctx_st, foreign_ctx_st, second_ctx_st ;
} job ;


static void *worker(void *p){
	job *j = p ;

	/* Another thread's ctx can't be used here. */
	ugly_val v ;
	j->foreign_ctx_st = ugly_get_member(j->f->ctx, j->f->mod, "echo", UGLY_ANY, &v) ;

	ugly_ctx *ctx ;
	j->ctx_st = ugly_ctx_new(&ctx) ;
	if (j->ctx_st != UGLY_OK){
		return NULL ;
	}
	ugly_ctx *again ;
	j->second_ctx_st = ugly_ctx_new(&again) ;

	/* A runtime owned by another thread can't be called from here. */
	j->call_st = ugly_invoke(ctx, j->counter, "inc", NULL, 0, UGLY_ANY, &v) ;

	/* But releasing is allowed from any thread, with or without a ctx. */
	ugly_release(ctx, j->counter) ;
	ugly_release(NULL, j->callable) ;

	ugly_ctx_delete(ctx) ;
	return NULL ;
}


/* UGLY_CAP_ANY_THREAD: threads with their own ctx, all in the shared runtime at once. */
#define NTHREADS 4
#define NCALLS 200

typedef struct {
	fixture *f ;
	int id ;
	ugly_rt *got ;               /* what ugly_rt_get gave this thread */
	int bad ;                    /* calls that failed or gave a wrong value */
	ugly_status st ;
} hammer_job ;


static void *hammer(void *p){
	hammer_job *w = p ;
	fixture mine = *w->f ;       /* the same language, this thread's ctx */
	if ((w->st = ugly_ctx_new(&mine.ctx)) != UGLY_OK){
		return NULL ;
	}
	ugly_handle mod = UGLY_NO_HANDLE, counter = UGLY_NO_HANDLE ;
	w->st = ugly_rt_get(mine.ctx, plugin_name(), &w->got) ;
	if (w->st == UGLY_OK){
		w->st = ugly_rt_import(mine.ctx, w->got, "conformance", &mod) ;
	}
	if ((w->st == UGLY_OK)&&((counter = make_counter(&mine, mod)) == UGLY_NO_HANDLE)){
		w->st = UGLY_ERR_INTERNAL ;
	}
	int i ;
	for (i = 0 ; (w->st == UGLY_OK)&&(i < NCALLS) ; i++){
		ugly_val arg = ugly_int(w->id * 100000 + i), out ;
		if ((fcall(&mine, mod, "echo", &arg, 1, UGLY_INT, &out) != UGLY_OK)||(out.as.i != arg.as.i)){
			w->bad++ ;
		}
		if ((ugly_invoke(mine.ctx, counter, "inc", NULL, 0, UGLY_INT, &out) != UGLY_OK)||(out.as.i != i + 1)){
			w->bad++ ;
		}
	}
	ugly_release(mine.ctx, counter) ;
	ugly_release(mine.ctx, mod) ;
	ugly_ctx_delete(mine.ctx) ;     /* drops this thread's share */
	return NULL ;
}


/* Holds a share of the runtime while the main thread deletes it. */
typedef struct {
	fixture *f ;
	sem_t ready, go ;
	ugly_status get_st, after_st ;
} holder_job ;


static void *holder(void *p){
	holder_job *h = p ;
	ugly_ctx *ctx ;
	ugly_rt *rt = NULL ;
	ugly_handle mod = UGLY_NO_HANDLE ;
	if ((h->get_st = ugly_ctx_new(&ctx)) == UGLY_OK){
		h->get_st = ugly_rt_get(ctx, plugin_name(), &rt) ;
		if (h->get_st == UGLY_OK){
			h->get_st = ugly_rt_import(ctx, rt, "conformance", &mod) ;
		}
	}
	sem_post(&h->ready) ;
	sem_wait(&h->go) ;               /* the main thread has deleted the runtime */
	if (h->get_st == UGLY_OK){
		ugly_val v ;
		h->after_st = ugly_get_member(ctx, mod, "echo", UGLY_HANDLE, &v) ;
		ugly_release(ctx, mod) ;
		ugly_ctx_delete(ctx) ;       /* the last share: frees the dead runtime */
	}
	return NULL ;
}


/* A runtime the host serializes: this thread takes its turn in it. */
typedef struct {
	ugly_handle counter ;
	ugly_status call_st ;
	int64_t count ;
} turn_job ;


static void *take_turn(void *p){
	turn_job *t = p ;
	ugly_ctx *ctx ;
	if ((t->call_st = ugly_ctx_new(&ctx)) == UGLY_OK){
		ugly_val v ;
		t->call_st = ugly_invoke(ctx, t->counter, "inc", NULL, 0, UGLY_INT, &v) ;
		t->count = v.as.i ;
		ugly_ctx_delete(ctx) ;
	}
	return NULL ;
}


static int freed ;
static void on_free(void *p){
	(void)p ;
	freed++ ;
}

static ugly_status noop(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)ctx ; (void)data ; (void)args ; (void)n ; (void)want ;
	*out = ugly_null() ;
	return UGLY_OK ;
}


int main(void){
	fixture f ;
	fx_setup(&f) ;
	ugly_ctx *ctx = f.ctx ;

	ugly_ctx *second ;
	is_status(ctx, ugly_ctx_new(&second), UGLY_ERR_USAGE, "a second ctx on the same thread: UGLY_ERR_USAGE") ;

	job j ;
	memset(&j, 0, sizeof(j)) ;
	j.f = &f ;
	j.counter = make_counter(&f, f.mod) ;
	ugly_callable_new(ctx, noop, NULL, on_free, &j.callable) ;
	int64_t before = finalized(&f, f.mod) ;

	pthread_t t ;
	pthread_create(&t, NULL, worker, &j) ;
	pthread_join(t, NULL) ;

	is_status(ctx, j.foreign_ctx_st, UGLY_ERR_WRONG_THREAD, "using another thread's ctx: UGLY_ERR_WRONG_THREAD") ;
	is_status(ctx, j.ctx_st, UGLY_OK, "the other thread gets its own ctx") ;
	is_status(ctx, j.second_ctx_st, UGLY_ERR_USAGE, "  ... and only one") ;
	int any = (ugly_lang_caps(f.lang) & UGLY_CAP_ANY_THREAD) != 0 ;
	if (any){
		is_status(ctx, j.call_st, UGLY_OK, "calling into another thread's runtime with UGLY_CAP_ANY_THREAD: UGLY_OK") ;
	}
	else {
		is_status(ctx, j.call_st, UGLY_ERR_WRONG_THREAD, "calling into another thread's runtime: UGLY_ERR_WRONG_THREAD") ;
	}

	/* The releases were deferred: they happen on the next entry into each home runtime. */
	is_int(freed, 0, "a release from another thread doesn't run the C callable's finalizer there") ;
	ugly_handle l ;
	ugly_list_new(ctx, NULL, 0, &l) ;
	int64_t n ;
	ugly_length(ctx, l, &n) ;          /* enters the host runtime */
	is_int(freed, 1, "  ... it runs on the next entry into its home runtime") ;
	ugly_release(ctx, l) ;
	is_int(finalized(&f, f.mod), before + 2, "the counter released from the other thread is finalized at home") ;

	if (any){
		hammer_job w[NTHREADS] ;
		pthread_t ts[NTHREADS] ;
		int i, bad = 0, all_ok = 1, shared = 1 ;
		for (i = 0 ; i < NTHREADS ; i++){
			memset(&w[i], 0, sizeof(w[i])) ;
			w[i].f = &f ;
			w[i].id = i + 1 ;
			pthread_create(&ts[i], NULL, hammer, &w[i]) ;
		}
		for (i = 0 ; i < NTHREADS ; i++){
			pthread_join(ts[i], NULL) ;
			all_ok &= (w[i].st == UGLY_OK) ;
			bad += w[i].bad ;
			shared &= (w[i].got == f.rt) ;
		}
		ok(all_ok, "%d threads, each with its own ctx, in the runtime at once", NTHREADS) ;
		if (! ok(bad == 0, "  ... %d calls each, every result right", 2 * NCALLS)){
			diag("  %d calls failed or gave a wrong value", bad) ;
		}
		ok(shared, "  ... ugly_rt_get gave every thread the shared runtime") ;

		/* Deleted while another thread holds a share: dead at once, freed with the last share. */
		holder_job h ;
		memset(&h, 0, sizeof(h)) ;
		h.f = &f ;
		sem_init(&h.ready, 0, 0) ;
		sem_init(&h.go, 0, 0) ;
		pthread_t t2 ;
		pthread_create(&t2, NULL, holder, &h) ;
		sem_wait(&h.ready) ;
		ugly_release(ctx, f.mod) ;
		f.mod = UGLY_NO_HANDLE ;
		is_status(ctx, ugly_rt_delete(ctx, f.rt), UGLY_OK, "deleting a runtime another thread holds a share of") ;
		f.rt = NULL ;
		sem_post(&h.go) ;
		pthread_join(t2, NULL) ;
		is_status(ctx, h.get_st, UGLY_OK, "  ... (that thread got it from ugly_rt_get)") ;
		is_status(ctx, h.after_st, UGLY_ERR_STALE_HANDLE, "  ... its handles are stale there at once") ;
		sem_destroy(&h.ready) ;
		sem_destroy(&h.go) ;
	}
	else {
		skip("plugin lacks UGLY_CAP_ANY_THREAD: concurrent threads") ;
		skip("plugin lacks UGLY_CAP_ANY_THREAD: deleting a shared runtime") ;

		/*
		 * A runtime the host serializes (HAProxy's lua-load): once the
		 * binding says so, other threads may enter it, one at a time. Here
		 * the turns are a join.
		 */
		turn_job t ;
		memset(&t, 0, sizeof(t)) ;
		t.counter = make_counter(&f, f.mod) ;
		ugly_rt_set_host_serialized(f.rt) ;
		pthread_t t3 ;
		pthread_create(&t3, NULL, take_turn, &t) ;
		pthread_join(t3, NULL) ;
		is_status(ctx, t.call_st, UGLY_OK, "a host-serialized runtime: another thread takes its turn in it") ;
		is_int(t.count, 1, "  ... and its call ran") ;
		int64_t fin = finalized(&f, f.mod) ;
		ugly_release(ctx, t.counter) ;     /* queued: the runtime is any-thread now */
		ugly_rt_drain(f.rt) ;              /* what a binding does at its entry points */
		is_int(finalized(&f, f.mod), fin + 2, "  ... and ugly_rt_drain runs the releases queued for it") ;
	}

	fx_teardown(&f) ;
	return done_testing() ;
}
