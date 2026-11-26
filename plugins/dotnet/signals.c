/*
 * Starting the CLR leaves the host's signals alone (ugly_plugin.h, init).
 * The CLR keeps its own handlers for the faults it turns into exceptions,
 * which call the host's for a fault outside managed code; its others
 * (SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGABRT) are put back.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "ugly.h"

static int n, failed ;

static void ok(int cond, const char *what){
	printf("%sok %d - %s\n", cond ? "" : "not ", ++n, what) ;
	failed |= ! cond ;
}

static sigjmp_buf env ;
static volatile sig_atomic_t caught ;

static void host(int s){
	caught = s ;
	siglongjmp(env, 1) ;
}

static void host_int(int s){
	(void)s ;
}

int main(void){
	static const int kept[] = { SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGABRT } ;
	struct sigaction a, before[sizeof(kept) / sizeof(kept[0])], after ;
	memset(&a, 0, sizeof(a)) ;
	a.sa_handler = host ;
	sigaction(SIGSEGV, &a, NULL) ;
	sigaction(SIGFPE, &a, NULL) ;
	a.sa_handler = host_int ;
	sigaction(SIGINT, &a, NULL) ;
	size_t i ;
	for (i = 0 ; i < sizeof(kept) / sizeof(kept[0]) ; i++){
		sigaction(kept[i], NULL, &before[i]) ;
	}

	ugly_ctx *ctx = NULL ;
	ugly_lang *lang = NULL ;
	ugly_rt *rt = NULL ;
	ugly_handle math ;
	ugly_ctx_new(&ctx) ;
	ugly_status st = ugly_lang_load(ctx, "dotnet", &lang) ;
	ok(st == UGLY_OK, "load dotnet (starts the CLR)") ;
	if (st != UGLY_OK){
		printf("Bail out! %s\n", ugly_error_message(ugly_last_error(ctx))) ;
		return 1 ;
	}
	ugly_rt_new(ctx, lang, &rt) ;
	ok(ugly_rt_import(ctx, rt, "System.Math", &math) == UGLY_OK, "import System.Math") ;

	for (i = 0 ; i < sizeof(kept) / sizeof(kept[0]) ; i++){
		char what[64] ;
		sigaction(kept[i], NULL, &after) ;
		snprintf(what, sizeof(what), "%s: the host's handler, unchanged", strsignal(kept[i])) ;
		ok(after.sa_handler == before[i].sa_handler, what) ;
	}

	/* A fault in C, outside managed code: the CLR's handler passes it on to the host's. */
	caught = 0 ;
	if (sigsetjmp(env, 1) == 0){
		*(volatile int *)NULL = 1 ;
	}
	ok(caught == SIGSEGV, "a SIGSEGV in C reaches the host's handler") ;
	caught = 0 ;
	volatile int zero = 0 ;
	if (sigsetjmp(env, 1) == 0){
		printf("# %d\n", 1 / zero) ;
	}
	ok(caught == SIGFPE, "a SIGFPE in C reaches the host's handler") ;

	ugly_val v, x = ugly_int(-7) ;
	st = ugly_invoke(ctx, math, "Abs", &x, 1, UGLY_INT, &v) ;
	ok((st == UGLY_OK)&&(v.as.i == 7), "and C# still works: Math.Abs(-7) == 7") ;

	ugly_release(ctx, math) ;
	ugly_rt_delete(ctx, rt) ;
	ugly_ctx_delete(ctx) ;
	printf("1..%d\n", n) ;
	return failed ;
}
