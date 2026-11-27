/*
 * ugly_rt_get for a language whose runtimes any thread may enter, and that
 * has one per process (Python's shape): threads asking for it at the same
 * moment, when there is none yet, must all get the same one. A test plugin,
 * registered here, takes its time making the runtime, so that they overlap.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ugly_plugin.h"

static int n_tests = 0, n_failed = 0 ;

static void ok(int good, const char *what){
	n_tests++ ;
	if (! good){
		n_failed++ ;
	}
	printf("%s %d - %s\n", good ? "ok" : "not ok", n_tests, what) ;
}


static int made, rt_token ;     /* runtimes made: written under the core's promise that one thread creates */

static ugly_status t_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	(void)ctx ; (void)rt ;
	struct timespec ts = { 0, 5 * 1000 * 1000 } ;
	nanosleep(&ts, NULL) ;
	__atomic_add_fetch(&made, 1, __ATOMIC_SEQ_CST) ;
	*native = &rt_token ;
	return UGLY_OK ;
}

static ugly_status t_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	(void)ctx ; (void)rt ; (void)native ;
	return UGLY_OK ;
}

static void t_release(ugly_rt *rt, void *ref){
	(void)rt ; (void)ref ;
}

static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = UGLY_CAP_ANY_THREAD,
	.name = "single",
	.rt_new = t_rt_new,
	.rt_delete = t_rt_delete,
	.release = t_release,
} ;


#define NTHREADS 8
#define ROUNDS 5

static pthread_barrier_t start, got ;
static ugly_status sts[NTHREADS] ;
static ugly_rt *rts[NTHREADS] ;

static void *worker(void *p){
	long i = (long)p ;
	ugly_ctx *ctx = NULL ;
	ugly_ctx_new(&ctx) ;
	pthread_barrier_wait(&start) ;
	sts[i] = ugly_rt_get(ctx, "single", &rts[i]) ;
	pthread_barrier_wait(&got) ;     /* every thread holds its share before any drops it */
	ugly_ctx_delete(ctx) ;
	return NULL ;
}


int main(void){
	ugly_ctx *ctx = NULL ;
	ugly_lang *lang = NULL ;
	ugly_ctx_new(&ctx) ;
	ok(ugly_lang_register(ctx, &plugin, &lang) == UGLY_OK, "a test language: any thread, one runtime per process") ;

	int round, all_ok = 1, all_same = 1 ;
	long i ;
	for (round = 0 ; round < ROUNDS ; round++){
		pthread_t t[NTHREADS] ;
		pthread_barrier_init(&start, NULL, NTHREADS) ;
		pthread_barrier_init(&got, NULL, NTHREADS) ;
		for (i = 0 ; i < NTHREADS ; i++){
			pthread_create(&t[i], NULL, worker, (void *)i) ;
		}
		for (i = 0 ; i < NTHREADS ; i++){
			pthread_join(t[i], NULL) ;
			all_ok &= (sts[i] == UGLY_OK) ;
			all_same &= (rts[i] != NULL)&&(rts[i] == rts[0]) ;
		}
		pthread_barrier_destroy(&start) ;
		pthread_barrier_destroy(&got) ;
	}
	ok(all_ok, "8 threads asking for its runtime at once, when there is none: none is refused") ;
	ok(all_same, "  ... and all get the same one") ;
	ok(made == ROUNDS, "  ... made once each time (the last share deletes it)") ;
	if (made != ROUNDS){
		printf("# %d runtimes made in %d rounds\n", made, ROUNDS) ;
	}

	ugly_lang_unload(ctx, lang) ;
	ugly_ctx_delete(ctx) ;
	printf("1..%d\n", n_tests) ;
	return n_failed != 0 ;
}
