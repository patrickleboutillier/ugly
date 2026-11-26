/*
 * The .NET plugin's skeleton, until it joins the conformance suite: the CLR
 * starts, a runtime comes and goes, operations are refused cleanly. Also
 * reports any signal handler that starting the CLR changed (TODO: the
 * contract says init leaves them alone).
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "ugly.h"

static int n, failed ;

static void ok(int cond, const char *what){
	printf("%sok %d - %s\n", cond ? "" : "not ", ++n, what) ;
	failed |= ! cond ;
}

int main(void){
	struct sigaction before[NSIG], after ;
	for (int s = 1 ; s < NSIG ; s++){
		sigaction(s, NULL, &before[s]) ;
	}

	ugly_ctx *ctx = NULL ;
	ugly_lang *lang = NULL ;
	ugly_rt *rt = NULL ;
	ugly_handle h ;
	ok(ugly_ctx_new(&ctx) == UGLY_OK, "ctx") ;
	ugly_status st = ugly_lang_load(ctx, "dotnet", &lang) ;
	ok(st == UGLY_OK, "load dotnet (starts the CLR)") ;
	if (st != UGLY_OK){
		printf("# %s\n", ugly_error_message(ugly_last_error(ctx))) ;
		printf("1..%d\n", n) ;
		return 1 ;
	}
	ok(ugly_rt_new(ctx, lang, &rt) == UGLY_OK, "runtime") ;
	ok(ugly_rt_import(ctx, rt, "System.Math", &h) == UGLY_ERR_NOT_SUPPORTED, "import refused, for now") ;
	printf("# %s\n", ugly_error_message(ugly_last_error(ctx))) ;
	ok(ugly_rt_delete(ctx, rt) == UGLY_OK, "runtime deleted") ;

	int changed = 0 ;
	for (int s = 1 ; s < NSIG ; s++){
		if ((sigaction(s, NULL, &after) == 0)&&(after.sa_handler != before[s].sa_handler)){
			printf("# signal %d (%s): handler changed\n", s, strsignal(s)) ;
			changed++ ;
		}
	}
	printf("%sok %d - signal handlers left alone # TODO the CLR chains its own\n", changed ? "not " : "", ++n) ;

	ugly_ctx_delete(ctx) ;
	printf("1..%d\n", n) ;
	return failed ;
}
