/* The error context: every API call reports errors here instead of throwing. */
#include "ugly_test.h"


int main(void){
	ugly_context *ctx = ugly_context_new() ;
	ok(ctx != NULL, "context created") ;
	ok(! ugly_context_has_error(ctx), "new context has no pending error") ;
	is_long(ugly_context_get_error(ctx), UGLY_OK, "new context error is UGLY_OK") ;
	is_string(ugly_context_get_error_msg(ctx), NULL, "new context error message is NULL") ;

	ugly_context_set_error(ctx, UGLY_INTERNAL_ERROR, NULL) ;
	ok(ugly_context_has_error(ctx), "error set without a message is pending") ;
	is_long(ugly_context_get_error(ctx), UGLY_INTERNAL_ERROR, "error is UGLY_INTERNAL_ERROR") ;
	is_string(ugly_context_get_error_msg(ctx), NULL, "error message is NULL") ;

	ugly_context_clear_error(ctx) ;
	ok(! ugly_context_has_error(ctx), "cleared context has no pending error") ;
	is_long(ugly_context_get_error(ctx), UGLY_OK, "cleared context error is UGLY_OK") ;
	is_string(ugly_context_get_error_msg(ctx), NULL, "cleared context error message is NULL") ;

	ugly_context_set_error(ctx, UGLY_RUNTIME_ERROR, "runtime %s %d", "error", 42) ;
	is_long(ugly_context_get_error(ctx), UGLY_RUNTIME_ERROR, "error is UGLY_RUNTIME_ERROR") ;
	is_string(ugly_context_get_error_msg(ctx), "runtime error 42", "error message is formatted") ;

	ugly_context_set_error(ctx, UGLY_LIBRARY_ERROR, "replaced") ;
	is_long(ugly_context_get_error(ctx), UGLY_LIBRARY_ERROR, "setting an error replaces the previous one") ;
	is_string(ugly_context_get_error_msg(ctx), "replaced", "... and its message") ;

	ugly_context_delete(ctx) ;
	return done_testing() ;
}
