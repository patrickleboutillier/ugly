/*
 * The Haskell plugin's skeleton, until it joins the conformance suite: the
 * RTS starts with an option, leaves every signal handler alone, runtimes
 * come and go (two at once, and one deleted by another thread), and modules
 * are found in libraries on the module path (the "path" option here: the
 * test's UGLY_HASKELL_PATH, unset). Values are values.c's.
 */
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ugly.h"

static int n, failed ;

static void ok(int cond, const char *what){
	printf("%sok %d - %s\n", cond ? "" : "not ", ++n, what) ;
	failed |= ! cond ;
}


static ugly_lang *lang ;

/* import 'name' into rt fails: not found, with this in the message. */
static int not_found(ugly_ctx *ctx, ugly_rt *rt, const char *name, const char *message){
	ugly_handle h ;
	ugly_status st = ugly_rt_import(ctx, rt, name, &h) ;
	const ugly_error *e = ugly_last_error(ctx) ;
	int good = (st == UGLY_ERR_NOT_FOUND)&&(e != NULL)&&(strcmp(ugly_error_type(e), "ModuleNotFound") == 0)
		&&(strstr(ugly_error_message(e), message) != NULL) ;
	printf("# %s\n", e != NULL ? ugly_error_message(e) : "no error") ;
	return good ;
}

/* import 'name' into rt works, and the module has n members. */
static int found(ugly_ctx *ctx, ugly_rt *rt, const char *name, int64_t n){
	ugly_handle h, names ;
	int64_t len = -1 ;
	if (ugly_rt_import(ctx, rt, name, &h) != UGLY_OK){
		printf("# %s\n", ugly_error_message(ugly_last_error(ctx))) ;
		return 0 ;
	}
	if (ugly_member_names(ctx, h, &names) == UGLY_OK){
		ugly_length(ctx, names, &len) ;
		ugly_release(ctx, names) ;
	}
	ugly_release(ctx, h) ;
	return len == n ;
}

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
	ok(ugly_ctx_new(&ctx) == UGLY_OK, "ctx") ;
	ok(ugly_configure(ctx, "haskell", "rts", "-N2") == UGLY_OK, "RTS options (rts)") ;
	const char *dir = getenv("UGLY_HASKELL_PATH") ;
	ok((dir != NULL)&&(ugly_configure(ctx, "haskell", "path", dir) == UGLY_OK), "the module path (path)") ;
	unsetenv("UGLY_HASKELL_PATH") ;
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
	ok(not_found(ctx, rt, "Data.List", "Could not find module \"Data.List\""), "import Data.List: not found (no library has it)") ;
	ok(found(ctx, rt, "Smoke", 49), "import Smoke: Smoke.so, on the path option") ;
	ok(found(ctx, rt2, "Smoke", 49), "and in another runtime") ;
	ok(found(ctx, rt, "Example", 13), "import Example: Example.so") ;
	ok(found(ctx, rt, "Example.Points", 2), "import Example.Points: Example.so too (the longest prefix)") ;
	ok(not_found(ctx, rt, "Example.Nope", "Could not find"), "import Example.Nope: Example.so doesn't have it") ;
	ok(not_found(ctx, rt, "Broken", "Could not load module \"Broken\""), "import Broken: Broken.so isn't a library") ;
	ok(not_found(ctx, rt, "../test/Smoke", "Could not find"), "import ../test/Smoke: a name, never a path") ;
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
