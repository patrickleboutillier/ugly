/*
 * libugly keeps its internals to itself: a host that exports functions with
 * the same names as the core's internal ones (this program is linked with
 * -rdynamic, as HAProxy and qjs are) must not take their place.
 */
#include <pthread.h>
#include "conformance.h"

static int taken ;

/* The core's own xstrdup takes NULL; a host's usually doesn't. */
char *xstrdup(const char *s){
	(void)s ;
	taken++ ;
	return NULL ;
}

int on_thread(pthread_t t){
	(void)t ;
	taken++ ;
	return 0 ;
}

int ctx_check(void *ctx){
	(void)ctx ;
	taken++ ;
	return UGLY_ERR_INTERNAL ;
}


int main(void){
	ugly_ctx *ctx = NULL ;
	ugly_status st = ugly_ctx_new(&ctx) ;
	ok(st == UGLY_OK, "ugly_ctx_new in a host that exports xstrdup, on_thread and ctx_check") ;
	if (st == UGLY_OK){
		st = ugly_raise(ctx, UGLY_ERR_TYPE, NULL, "boom", UGLY_NO_HANDLE) ;
		const ugly_error *e = ugly_last_error(ctx) ;
		ok((st == UGLY_ERR_TYPE)&&(e != NULL)&&(strcmp(ugly_error_type(e), "") == 0)
			&&(strcmp(ugly_error_message(e), "boom") == 0), "an error with a NULL type is recorded by the core's own functions") ;
		ugly_handle l = UGLY_NO_HANDLE ;
		int64_t n = -1 ;
		ok((ugly_list_new(ctx, NULL, 0, &l) == UGLY_OK)&&(ugly_length(ctx, l, &n) == UGLY_OK)&&(n == 0),
			"a call through the core works") ;
		ugly_release(ctx, l) ;
		ugly_ctx_delete(ctx) ;
	}
	ok(taken == 0, "the host's functions were never called by libugly") ;
	return done_testing() ;
}
