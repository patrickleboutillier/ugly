/* Errors: structure, propagation across runtimes, and rethrow identity (§5). */
#include "conformance.h"


static int same(fixture *f, ugly_handle a, ugly_handle b){
	bool s = false ;
	return (ugly_same(f->ctx, a, b, &s) == UGLY_OK) && s ;
}


/* A C callback that raises, with a payload (a core map) it keeps a reference to. */
static ugly_handle c_payload ;
static ugly_status c_raise(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)data ; (void)args ; (void)n ; (void)want ; (void)out ;
	ugly_handle p ;
	ugly_dup(ctx, c_payload, &p) ;
	return ugly_raise(ctx, UGLY_ERR_RAISED, "CError", "raised in C", p) ;
}


/* A C callback that calls rt2.raise_error(msg): an error from a third runtime. */
static ugly_handle raise2 ;
static ugly_status c_via_rt2(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)data ;
	return ugly_call(ctx, raise2, args, n, want, out) ;
}


int main(void){
	fixture f ;
	fx_setup(&f) ;
	ugly_ctx *ctx = f.ctx ;
	ugly_val v ;
	ugly_status st ;
	const ugly_error *e ;

	/* A language exception: structure and payload. */
	ugly_val msg = ugly_str("boom", 4) ;
	st = fcall(&f, f.mod, "raise_error", &msg, 1, UGLY_ANY, &v) ;
	is_status(ctx, st, UGLY_ERR_RAISED, "raise_error(boom): UGLY_ERR_RAISED") ;
	e = ugly_last_error(ctx) ;
	ok(e != NULL, "  ... with an error") ;
	ugly_handle p1 = UGLY_NO_HANDLE ;
	if (e != NULL){
		is_str(ugly_error_language(e), plugin_name(), "  ... language") ;
		is_str(ugly_error_type(e), "ConformanceError", "  ... type") ;
		is_str(ugly_error_message(e), "boom", "  ... message") ;
		st = ugly_error_payload(ctx, e, &p1) ;
		ok((st == UGLY_OK)&&(p1 != UGLY_NO_HANDLE), "  ... and a payload") ;
		ok(ugly_last_error(ctx) == e, "  ... reading the payload keeps the error") ;
	}
	st = ugly_get_member(ctx, p1, "message", UGLY_STRING, &v) ;
	is_status(ctx, st, UGLY_OK, "the payload is the exception object") ;
	is_str(sval(&v), "boom", "  ... its message member is boom") ;
	ugly_val_release(ctx, &v) ;
	ok(ugly_last_error(ctx) == NULL, "a successful call clears the error") ;

	/* Rethrown at home, the payload is the identical object. */
	ugly_val arg = ugly_hval(p1) ;
	st = fcall(&f, f.mod, "rethrow", &arg, 1, UGLY_ANY, &v) ;
	is_status(ctx, st, UGLY_ERR_RAISED, "rethrow(payload): UGLY_ERR_RAISED") ;
	ugly_handle p2 = UGLY_NO_HANDLE ;
	if ((e = ugly_last_error(ctx)) != NULL){
		ugly_error_payload(ctx, e, &p2) ;
	}
	ok(same(&f, p1, p2), "  ... and the payload is the same object") ;
	ugly_release(ctx, p2) ;

	/* An error from a C callback, caught in the language: the result is the C payload itself. */
	ugly_map_new(ctx, &c_payload) ;
	ugly_handle h_raise ;
	ugly_callable_new(ctx, c_raise, NULL, NULL, &h_raise) ;
	arg = ugly_hval(h_raise) ;
	st = fcall(&f, f.mod, "catch_and_return", &arg, 1, UGLY_HANDLE, &v) ;
	is_status(ctx, st, UGLY_OK, "catch_and_return(C callback that raises)") ;
	ok((st == UGLY_OK)&&same(&f, v.as.h, c_payload), "  ... returns the identical payload") ;
	ugly_val_release(ctx, &v) ;

	/* Uncaught, a C error comes out as it went in. */
	ugly_val a[] = { ugly_hval(h_raise), ugly_null() } ;
	st = fcall(&f, f.mod, "apply", a, 2, UGLY_ANY, &v) ;
	is_status(ctx, st, UGLY_ERR_RAISED, "apply(C callback that raises): propagates") ;
	if ((e = ugly_last_error(ctx)) != NULL){
		is_str(ugly_error_language(e), "c", "  ... language c") ;
		is_str(ugly_error_type(e), "CError", "  ... type CError") ;
	}
	ugly_release(ctx, h_raise) ;
	ugly_release(ctx, c_payload) ;

	/* Across three runtimes: rt1 -> C -> rt2, which raises. */
	ugly_rt *rt2 ;
	ugly_handle mod2 ;
	if (fx_second(&f, &rt2, &mod2, "across three runtimes")){
		ugly_val t ;
		ugly_get_member(ctx, mod2, "raise_error", UGLY_HANDLE, &t) ;
		raise2 = t.as.h ;
		ugly_handle h_via ;
		ugly_callable_new(ctx, c_via_rt2, NULL, NULL, &h_via) ;
		ugly_val b[] = { ugly_hval(h_via), ugly_str("deep", 4) } ;
		st = fcall(&f, f.mod, "apply", b, 2, UGLY_ANY, &v) ;
		is_status(ctx, st, UGLY_ERR_RAISED, "rt1 -> C -> rt2 raises: propagates to the host") ;
		if ((e = ugly_last_error(ctx)) != NULL){
			is_str(ugly_error_type(e), "ConformanceError", "  ... type preserved") ;
			is_str(ugly_error_message(e), "deep", "  ... message preserved") ;
			ugly_handle p3 = UGLY_NO_HANDLE ;
			ugly_error_payload(ctx, e, &p3) ;
			st = ugly_get_member(ctx, p3, "message", UGLY_STRING, &v) ;
			ok((st == UGLY_OK)&&(strcmp(sval(&v), "deep") == 0), "  ... payload is rt2's exception object") ;
			ugly_val_release(ctx, &v) ;
			ugly_release(ctx, p3) ;
		}
		ugly_release(ctx, h_via) ;
		ugly_release(ctx, raise2) ;
		ugly_release(ctx, mod2) ;
		ugly_rt_delete(ctx, rt2) ;
	}

	ugly_release(ctx, p1) ;
	fx_teardown(&f) ;
	return done_testing() ;
}
