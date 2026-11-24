/* Threads: runtimes and ctxs belong to one thread; releases from other threads are deferred (§6, §3). */
#include <pthread.h>
#include "conformance.h"


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
	is_status(ctx, j.call_st, UGLY_ERR_WRONG_THREAD, "calling into another thread's runtime: UGLY_ERR_WRONG_THREAD") ;

	/* The releases were deferred: they happen on the next entry into each home runtime. */
	is_int(freed, 0, "a release from another thread doesn't run the C callable's finalizer there") ;
	ugly_handle l ;
	ugly_list_new(ctx, NULL, 0, &l) ;
	int64_t n ;
	ugly_length(ctx, l, &n) ;          /* enters the host runtime */
	is_int(freed, 1, "  ... it runs on the next entry into its home runtime") ;
	ugly_release(ctx, l) ;
	is_int(finalized(&f, f.mod), before + 2, "the counter released from the other thread is finalized at home") ;

	fx_teardown(&f) ;
	return done_testing() ;
}
