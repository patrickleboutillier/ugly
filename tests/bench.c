/*
 * Benchmarks: what a call through UGLY costs (TODO.md, "Benchmarks").
 *
 * Every language built (UGLY_TEST_LANGS) is loaded into this one process,
 * with its "conformance" fixtures, and the program prints Markdown tables:
 *
 *  1. C -> L: ugly_call on L's echo, with an int, a 16-byte string, and a
 *     handle (L's own object, which goes home and comes back).
 *  2. L natively: L's bench_self(n), a loop in L calling its own echo, for
 *     the cost of the same call without UGLY.
 *  3. A -> B: A's bench(B.echo, n), a loop in A calling B's echo through a
 *     proxy, for every ordered pair.
 *  4. Threads: calls per second from 1 to N threads at once: into C
 *     callables (the core alone: its handle table has one lock), into a Lua
 *     runtime per thread, and into the one Python (the GIL).
 *
 * Each measurement doubles its count until it takes BENCH_SECONDS (0.3 by
 * default), so the first rounds warm up the JITs. Run with
 * meson test -C build --benchmark --verbose, or directly.
 */
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "conformance.h"

static const char *const all[] = { "mock", "lua", "python", "js", "perl5", "java", "dotnet" } ;
#define NALL (sizeof(all) / sizeof(all[0]))

typedef struct lang {
	const char *name ;
	ugly_lang *lang ;
	ugly_rt *rt ;
	ugly_handle mod, echo, bench, bench_self, obj ;
} lang ;

static ugly_ctx *ctx ;
static lang langs[NALL] ;
static size_t nlangs ;
static double target = 0.3 ;


static double now(void){
	struct timespec t ;
	clock_gettime(CLOCK_MONOTONIC, &t) ;
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9 ;
}


static int built(const char *name){
	const char *l = getenv("UGLY_TEST_LANGS") ;
	if ((l == NULL)||(*l == '\0')){
		return strcmp(name, "mock") == 0 ;
	}
	size_t n = strlen(name) ;
	for (const char *p = l ; (p = strstr(p, name)) != NULL ; p += n){
		if (((p == l)||(p[-1] == ','))&&((p[n] == ',')||(p[n] == '\0'))){
			return 1 ;
		}
	}
	return 0 ;
}


static void fail(const char *what){
	const ugly_error *e = ugly_last_error(ctx) ;
	fprintf(stderr, "bench: %s: %s\n", what, e ? ugly_error_message(e) : "?") ;
	exit(1) ;
}


static ugly_handle member(ugly_handle mod, const char *name){
	ugly_val v ;
	if (ugly_get_member(ctx, mod, name, UGLY_HANDLE, &v) != UGLY_OK){
		fail(name) ;
	}
	return v.as.h ;
}


/* ---------------------------------------------------------------- */
/* Timing                                                           */
/* ---------------------------------------------------------------- */

typedef ugly_status (*run_fn)(void *data, int64_t n) ;

/* Nanoseconds per iteration of run(data, n), with n doubled until a run takes 'target' seconds. */
static double measure(run_fn run, void *data){
	int64_t n = 100 ;
	for (;;){
		double t0 = now() ;
		if (run(data, n) != UGLY_OK){
			fail("a measured run") ;
		}
		double dt = now() - t0 ;
		if ((dt >= target)||(n >= ((int64_t)1 << 40))){
			return dt * 1e9 / (double)n ;
		}
		n *= (dt < target / 16 ? 8 : 2) ;
	}
}


/* C -> L: n calls of echo(arg). */
typedef struct { ugly_handle fn ; ugly_val arg ; } c_call ;

static ugly_status run_c_call(void *data, int64_t n){
	c_call *c = data ;
	int64_t i ;
	for (i = 0 ; i < n ; i++){
		ugly_val out ;
		ugly_status st = ugly_call(ctx, c->fn, &c->arg, 1, UGLY_ANY, &out) ;
		if (st != UGLY_OK){
			return st ;
		}
		ugly_val_release(ctx, &out) ;
	}
	return UGLY_OK ;
}


/* A loop inside a language: bench(f, n) or bench_self(n), one call. */
typedef struct { ugly_handle fn ; ugly_handle f ; } loop ;

static ugly_status run_loop(void *data, int64_t n){
	loop *l = data ;
	ugly_val args[2], out ;
	size_t nargs = 0 ;
	if (l->f != UGLY_NO_HANDLE){
		args[nargs++] = ugly_hval(l->f) ;
	}
	args[nargs++] = ugly_int(n) ;
	ugly_status st = ugly_call(ctx, l->fn, args, nargs, UGLY_ANY, &out) ;
	if (st == UGLY_OK){
		ugly_val_release(ctx, &out) ;
	}
	return st ;
}


static void cell(double ns){
	if (ns >= 10000){
		printf(" %.0f µs |", ns / 1000) ;
	}
	else if (ns >= 100){
		printf(" %.0f ns |", ns) ;
	}
	else {
		printf(" %.1f ns |", ns) ;
	}
	fflush(stdout) ;
}


/* ---------------------------------------------------------------- */
/* Threads                                                          */
/* ---------------------------------------------------------------- */

typedef enum { T_C, T_LUA, T_PYTHON } tkind ;

typedef struct {
	tkind kind ;
	int64_t calls ;          /* made, in 'target' seconds */
	pthread_barrier_t *start ;
	double elapsed ;
	int failed ;
} tjob ;

static ugly_status add1(ugly_ctx *c, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)c ; (void)data ; (void)n ; (void)want ;
	*out = ugly_int(args[0].as.i + 1) ;
	return UGLY_OK ;
}


static void *worker(void *p){
	tjob *j = p ;
	ugly_ctx *c = NULL ;
	ugly_rt *rt = NULL ;
	ugly_lang *lang = NULL ;
	ugly_handle fn = UGLY_NO_HANDLE, mod = UGLY_NO_HANDLE ;
	ugly_ctx_new(&c) ;
	if (j->kind == T_C){
		ugly_callable_new(c, add1, NULL, NULL, &fn) ;
	}
	else {
		ugly_status st = UGLY_OK ;
		if (j->kind == T_LUA){
			st = ugly_lang_load(c, "lua", &lang) ;
			if (st == UGLY_OK){
				st = ugly_rt_new(c, lang, &rt) ;
			}
		}
		else {
			st = ugly_rt_get(c, "python", &rt) ;
		}
		ugly_val v ;
		if ((st == UGLY_OK)&&(ugly_rt_import(c, rt, "conformance", &mod) == UGLY_OK)
			&&(ugly_get_member(c, mod, "echo", UGLY_HANDLE, &v) == UGLY_OK)){
			fn = v.as.h ;
		}
	}
	j->failed = (fn == UGLY_NO_HANDLE) ;
	pthread_barrier_wait(j->start) ;
	double t0 = now(), t ;
	int64_t i = 0 ;
	do {
		int k ;
		for (k = 0 ; (k < 256)&&(! j->failed) ; k++, i++){
			ugly_val arg = ugly_int(i), out ;
			if (ugly_call(c, fn, &arg, 1, UGLY_ANY, &out) != UGLY_OK){
				j->failed = 1 ;
			}
			ugly_val_release(c, &out) ;
		}
		t = now() - t0 ;
	} while ((t < target)&&(! j->failed)) ;
	j->calls = i ;
	j->elapsed = t ;
	ugly_release(c, fn) ;
	ugly_release(c, mod) ;
	if (rt != NULL && lang != NULL){
		ugly_rt_delete(c, rt) ;
		ugly_lang_unload(c, lang) ;
	}
	ugly_ctx_delete(c) ;
	return NULL ;
}


/* Total calls per second, with 'threads' threads calling for 'target' seconds each. */
static double throughput(tkind kind, int threads){
	pthread_t t[64] ;
	tjob jobs[64] ;
	pthread_barrier_t start ;
	pthread_barrier_init(&start, NULL, (unsigned)threads) ;
	int i ;
	for (i = 0 ; i < threads ; i++){
		jobs[i] = (tjob){ kind, 0, &start, 0, 0 } ;
		pthread_create(&t[i], NULL, worker, &jobs[i]) ;
	}
	double rate = 0 ;
	int failed = 0 ;
	for (i = 0 ; i < threads ; i++){
		pthread_join(t[i], NULL) ;
		failed |= jobs[i].failed ;
		rate += (double)jobs[i].calls / jobs[i].elapsed ;
	}
	pthread_barrier_destroy(&start) ;
	return failed ? -1 : rate ;
}


static void thread_row(const char *what, tkind kind, const int *counts, int ncounts){
	printf("| %s |", what) ;
	int i ;
	for (i = 0 ; i < ncounts ; i++){
		double r = throughput(kind, counts[i]) ;
		if (r < 0){
			printf(" failed |") ;
		}
		else {
			printf(" %.2f M/s |", r / 1e6) ;
		}
		fflush(stdout) ;
	}
	printf("\n") ;
}


/* ---------------------------------------------------------------- */

int main(void){
	const char *s = getenv("BENCH_SECONDS") ;
	if ((s != NULL)&&(atof(s) > 0)){
		target = atof(s) ;
	}
	if (ugly_ctx_new(&ctx) != UGLY_OK){
		fail("ctx") ;
	}
	size_t i, j ;
	for (i = 0 ; i < NALL ; i++){
		if (! built(all[i])){
			continue ;
		}
		lang *l = &langs[nlangs] ;
		l->name = all[i] ;
		if ((ugly_lang_load(ctx, l->name, &l->lang) != UGLY_OK)||(ugly_rt_new(ctx, l->lang, &l->rt) != UGLY_OK)
			||(ugly_rt_import(ctx, l->rt, "conformance", &l->mod) != UGLY_OK)){
			fail(l->name) ;
		}
		l->echo = member(l->mod, "echo") ;
		l->bench = member(l->mod, "bench") ;
		l->bench_self = member(l->mod, "bench_self") ;
		ugly_val v ;
		ugly_handle mk = member(l->mod, "make_counter") ;
		if (ugly_call(ctx, mk, NULL, 0, UGLY_HANDLE, &v) != UGLY_OK){
			fail("make_counter") ;
		}
		l->obj = v.as.h ;
		ugly_release(ctx, mk) ;
		nlangs++ ;
	}

	char host[128] = "?" ;
	gethostname(host, sizeof(host)) ;
	printf("UGLY benchmarks: %zu languages, %.2f s per measurement, %ld CPUs\n\n", nlangs, target, sysconf(_SC_NPROCESSORS_ONLN)) ;

	printf("### C calling each language (ns per call to `echo`)\n\n") ;
	printf("| Language | int | 16-byte string | its own object | natively, in the language |\n|---|---|---|---|---|\n") ;
	for (i = 0 ; i < nlangs ; i++){
		lang *l = &langs[i] ;
		printf("| %s |", l->name) ;
		c_call c = { l->echo, ugly_int(42) } ;
		cell(measure(run_c_call, &c)) ;
		c.arg = ugly_str("sixteen bytes!!!", 16) ;
		cell(measure(run_c_call, &c)) ;
		c.arg = ugly_hval(l->obj) ;
		cell(measure(run_c_call, &c)) ;
		loop self = { l->bench_self, UGLY_NO_HANDLE } ;
		cell(measure(run_loop, &self)) ;
		printf("\n") ;
	}

	printf("\n### Each language calling each other one (ns per call: row's loop calling column's `echo`)\n\n| Caller \\ callee |") ;
	for (j = 0 ; j < nlangs ; j++){
		printf(" %s |", langs[j].name) ;
	}
	printf("\n|---|") ;
	for (j = 0 ; j < nlangs ; j++){
		printf("---|") ;
	}
	printf("\n") ;
	for (i = 0 ; i < nlangs ; i++){
		printf("| %s |", langs[i].name) ;
		for (j = 0 ; j < nlangs ; j++){
			loop l = { langs[i].bench, langs[j].echo } ;
			cell(measure(run_loop, &l)) ;
		}
		printf("\n") ;
	}

	long cpus = sysconf(_SC_NPROCESSORS_ONLN) ;
	int counts[8], ncounts = 0, k ;
	for (k = 1 ; (k <= cpus)&&(ncounts < 8) ; k *= 2){
		counts[ncounts++] = k ;
	}
	if (counts[ncounts - 1] != cpus){
		counts[ncounts++] = (int)cpus ;
	}
	printf("\n### Threads (total calls per second)\n\n| Each thread calls |") ;
	for (k = 0 ; k < ncounts ; k++){
		printf(" %d thread%s |", counts[k], counts[k] > 1 ? "s" : "") ;
	}
	printf("\n|---|") ;
	for (k = 0 ; k < ncounts ; k++){
		printf("---|") ;
	}
	printf("\n") ;
	thread_row("a C callable (the core alone)", T_C, counts, ncounts) ;
	if (built("lua")){
		thread_row("its own Lua runtime", T_LUA, counts, ncounts) ;
	}
	if (built("python")){
		thread_row("the one Python (the GIL)", T_PYTHON, counts, ncounts) ;
	}

	for (i = 0 ; i < nlangs ; i++){
		lang *l = &langs[i] ;
		ugly_release(ctx, l->echo) ;
		ugly_release(ctx, l->bench) ;
		ugly_release(ctx, l->bench_self) ;
		ugly_release(ctx, l->obj) ;
		ugly_release(ctx, l->mod) ;
	}
	for (i = nlangs ; i-- > 0 ; ){
		ugly_rt_delete(ctx, langs[i].rt) ;
		ugly_lang_unload(ctx, langs[i].lang) ;
	}
	ugly_ctx_delete(ctx) ;
	return 0 ;
}
