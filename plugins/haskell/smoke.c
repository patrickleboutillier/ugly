/*
 * The Haskell plugin's skeleton, until it joins the conformance suite: the
 * RTS starts with an option, leaves every signal handler alone, runtimes
 * come and go (two at once, and one deleted by another thread), and
 * operations are refused cleanly.
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "ugly.h"

static int n, failed ;

static void ok(int cond, const char *what){
	printf("%sok %d - %s\n", cond ? "" : "not ", ++n, what) ;
	failed |= ! cond ;
}


static ugly_lang *lang ;

/* UGLY_CAP_ANY_THREAD: another thread deletes a runtime this one made, and makes and deletes its own. */
static void *other_thread(void *arg){
	ugly_rt *rt = arg, *mine = NULL ;
	ugly_ctx *ctx = NULL ;
	int good = ugly_ctx_thread(&ctx) == UGLY_OK ;
	good = good && ugly_rt_delete(ctx, rt) == UGLY_OK ;
	good = good && ugly_rt_new(ctx, lang, &mine) == UGLY_OK ;
	good = good && ugly_rt_delete(ctx, mine) == UGLY_OK ;
	return good ? arg : NULL ;
}


int main(void){
	setvbuf(stdout, NULL, _IOLBF, 0) ;     /* a hang still shows how far it got */
	struct sigaction before[NSIG], after ;
	for (int s = 1 ; s < NSIG ; s++){
		sigaction(s, NULL, &before[s]) ;
	}

	ugly_ctx *ctx = NULL ;
	ugly_rt *rt = NULL, *rt2 = NULL ;
	ugly_handle h ;
	ok(ugly_ctx_new(&ctx) == UGLY_OK, "ctx") ;
	ok(ugly_configure(ctx, "haskell", "rts", "-N2") == UGLY_OK, "RTS options (rts)") ;
	ugly_status st = ugly_lang_load(ctx, "haskell", &lang) ;
	ok(st == UGLY_OK, "load haskell (starts the RTS)") ;
	if (st != UGLY_OK){
		printf("# %s\n", ugly_error_message(ugly_last_error(ctx))) ;
		printf("1..%d\n", n) ;
		return 1 ;
	}
	uint32_t caps = ugly_lang_caps(lang) ;
	ok((caps & UGLY_CAP_ANY_THREAD) && (caps & UGLY_CAP_MULTI_RUNTIME) && ! (caps & UGLY_CAP_RESTARTABLE),
		"any thread, several runtimes, not restartable") ;

	ok(ugly_rt_new(ctx, lang, &rt) == UGLY_OK, "runtime") ;
	ok(ugly_rt_new(ctx, lang, &rt2) == UGLY_OK, "a second runtime, at the same time") ;
	ok(ugly_rt_import(ctx, rt, "Data.List", &h) == UGLY_ERR_NOT_IMPLEMENTED, "import refused, for now") ;
	printf("# %s\n", ugly_error_message(ugly_last_error(ctx))) ;
	ok(ugly_rt_delete(ctx, rt2) == UGLY_OK, "second runtime deleted") ;

	pthread_t t ;
	void *result = NULL ;
	pthread_create(&t, NULL, other_thread, rt) ;
	pthread_join(t, &result) ;
	ok(result == rt, "another thread deletes the first runtime, makes and deletes its own") ;

	int changed = 0 ;
	for (int s = 1 ; s < NSIG ; s++){
		if ((sigaction(s, NULL, &after) == 0)&&(after.sa_handler != before[s].sa_handler)){
			printf("# signal %d (%s): handler changed\n", s, strsignal(s)) ;
			changed++ ;
		}
	}
	ok(! changed, "signal handlers left alone") ;

	ugly_ctx_delete(ctx) ;
	printf("1..%d\n", n) ;
	return failed ;
}
