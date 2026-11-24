/*
 * Pairwise: two different languages in one process (contract.md section 9).
 * This is where "2N adapters compose" is actually checked: language A
 * (UGLY_TEST_PLUGIN) and language B (UGLY_TEST_PEER) use each other's values,
 * call each other, and raise through each other, with only the core between
 * them.
 */
#include "conformance.h"


static const char *peer_name(void){
	const char *p = getenv("UGLY_TEST_PEER") ;
	return ((p != NULL)&&(p[0] != '\0')) ? p : "mock" ;
}


static int same(ugly_ctx *ctx, ugly_handle a, ugly_handle b){
	bool s = false ;
	return (ugly_same(ctx, a, b, &s) == UGLY_OK) && s ;
}


static ugly_handle member(ugly_ctx *ctx, ugly_handle mod, const char *name){
	ugly_val v ;
	return (ugly_get_member(ctx, mod, name, UGLY_HANDLE, &v) == UGLY_OK ? v.as.h : UGLY_NO_HANDLE) ;
}


/* B.raise_error(msg), as a C callable that A can call. */
static ugly_handle b_raise ;
static ugly_status call_b_raise(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)data ; (void)n ;
	ugly_val msg = ugly_str("from B", 6) ;
	(void)args ;
	return ugly_call(ctx, b_raise, &msg, 1, want, out) ;
}


/* Recursion that alternates languages: A.apply -> C -> B.apply -> C -> A.apply ... */
typedef struct { fixture *f ; ugly_handle self ; ugly_handle apply[2] ; } alt ;
static ugly_status alternate(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)want ;
	alt *a = data ;
	if ((n != 1)||(args[0].kind != UGLY_INT)){
		return ugly_raise(ctx, UGLY_ERR_TYPE, "TypeError", "alternate wants an int", UGLY_NO_HANDLE) ;
	}
	int64_t k = args[0].as.i ;
	if (k == 0){
		*out = ugly_int(0) ;
		return UGLY_OK ;
	}
	ugly_val call[] = { ugly_hval(a->self), ugly_int(k - 1) } ;
	ugly_val v ;
	ugly_status st = ugly_call(ctx, a->apply[k % 2], call, 2, UGLY_INT, &v) ;
	if (st == UGLY_OK){
		*out = ugly_int(v.as.i + 1) ;
	}
	return st ;
}


int main(void){
	fixture f ;
	fx_setup(&f) ;           /* language A */
	ugly_ctx *ctx = f.ctx ;
	ugly_val v ;
	ugly_status st ;

	ugly_lang *lb ;
	ugly_rt *rb ;
	ugly_handle mb ;
	st = ugly_lang_load(ctx, peer_name(), &lb) ;
	if (st == UGLY_OK){
		st = ugly_rt_new(ctx, lb, &rb) ;
	}
	if (st == UGLY_OK){
		st = ugly_rt_import(ctx, rb, "conformance", &mb) ;
	}
	if (st != UGLY_OK){
		printf("Bail out! can't set up peer %s\n", peer_name()) ;
		return 1 ;
	}
	diag("A = %s, B = %s", plugin_name(), peer_name()) ;
	ugly_handle a_echo = member(ctx, f.mod, "echo"), b_echo = member(ctx, mb, "echo") ;

	/* Values cross A -> B -> A intact. */
	ugly_val x[] = { ugly_hval(b_echo), ugly_str("h\xc3\xa9llo", 6) } ;
	st = fcall(&f, f.mod, "apply", x, 2, UGLY_STRING, &v) ;
	is_status(ctx, st, UGLY_OK, "A.apply(B.echo, string)") ;
	is_str(sval(&v), "h\xc3\xa9llo", "  ... the string comes back intact") ;
	ugly_val_release(ctx, &v) ;
	x[1] = ugly_double(-0.25) ;
	st = fcall(&f, f.mod, "apply", x, 2, UGLY_DOUBLE, &v) ;
	ok((st == UGLY_OK)&&(v.as.d == -0.25), "A.apply(B.echo, double)") ;

	/* A B object inside A is a proxy, and comes back as the original. */
	ugly_handle bc = make_counter(&f, mb) ;
	ugly_val arg = ugly_hval(bc) ;
	st = fcall(&f, f.mod, "kind_of", &arg, 1, UGLY_STRING, &v) ;
	is_status(ctx, st, UGLY_OK, "A.kind_of(B counter)") ;
	is_str(sval(&v), "proxy", "  ... a proxy in A") ;
	ugly_val_release(ctx, &v) ;
	st = fcall(&f, f.mod, "echo", &arg, 1, UGLY_HANDLE, &v) ;
	ok((st == UGLY_OK)&&same(ctx, v.as.h, bc), "A.echo(B counter) returns B's counter itself") ;
	ugly_val_release(ctx, &v) ;
	ugly_val y[] = { ugly_hval(a_echo), ugly_hval(bc) } ;
	ugly_handle b_apply = member(ctx, mb, "apply") ;
	st = ugly_call(ctx, b_apply, y, 2, UGLY_HANDLE, &v) ;
	ok((st == UGLY_OK)&&same(ctx, v.as.h, bc), "B.apply(A.echo, B counter): home again, the same object") ;
	ugly_val_release(ctx, &v) ;
	ugly_release(ctx, b_apply) ;
	st = ugly_invoke(ctx, bc, "inc", NULL, 0, UGLY_INT, &v) ;
	ok((st == UGLY_OK)&&(v.as.i == 1), "the counter still works in B") ;

	/* An exception from B, caught in A: the result is B's exception object. */
	b_raise = member(ctx, mb, "raise_error") ;
	ugly_handle cb ;
	ugly_callable_new(ctx, call_b_raise, NULL, NULL, &cb) ;
	arg = ugly_hval(cb) ;
	st = fcall(&f, f.mod, "catch_and_return", &arg, 1, UGLY_HANDLE, &v) ;
	is_status(ctx, st, UGLY_OK, "A.catch_and_return(f), f raising in B") ;
	ugly_handle exc = v.as.h ;
	ugly_val m ;
	st = ugly_get_member(ctx, exc, "message", UGLY_STRING, &m) ;
	ok((st == UGLY_OK)&&(strcmp(sval(&m), "from B") == 0), "  ... B's exception object, message intact") ;
	ugly_val_release(ctx, &m) ;

	/* A rethrows B's exception: what comes out is B's object, not a proxy of it. */
	arg = ugly_hval(exc) ;
	st = fcall(&f, f.mod, "rethrow", &arg, 1, UGLY_ANY, &v) ;
	is_status(ctx, st, UGLY_ERR_RAISED, "A.rethrow(B exception)") ;
	ugly_handle p = UGLY_NO_HANDLE ;
	const ugly_error *e = ugly_last_error(ctx) ;
	if (e != NULL){
		ugly_error_payload(ctx, e, &p) ;
	}
	ok(same(ctx, p, exc), "  ... the payload is B's exception object itself") ;
	ugly_release(ctx, p) ;
	ugly_release(ctx, exc) ;

	/* Uncaught, B's error crosses A unchanged. */
	ugly_val z[] = { ugly_hval(cb), ugly_null() } ;
	st = fcall(&f, f.mod, "apply", z, 2, UGLY_ANY, &v) ;
	is_status(ctx, st, UGLY_ERR_RAISED, "A.apply(f), f raising in B: propagates") ;
	if ((e = ugly_last_error(ctx)) != NULL){
		is_str(ugly_error_language(e), peer_name(), "  ... still B's error") ;
		is_str(ugly_error_type(e), "ConformanceError", "  ... type intact") ;
	}
	ugly_release(ctx, cb) ;

	/* Recursion alternating languages: A -> C -> B -> C -> A ... 50 levels. */
	alt a = { &f, UGLY_NO_HANDLE, { member(ctx, f.mod, "apply"), member(ctx, mb, "apply") } } ;
	ugly_callable_new(ctx, alternate, &a, NULL, &a.self) ;
	ugly_val fifty = ugly_int(50) ;
	st = ugly_call(ctx, a.self, &fifty, 1, UGLY_INT, &v) ;
	is_status(ctx, st, UGLY_OK, "recursion alternating A and B, 50 levels") ;
	is_int(v.as.i, 50, "  ... returns 50") ;
	ugly_release(ctx, a.self) ;
	ugly_release(ctx, a.apply[0]) ;
	ugly_release(ctx, a.apply[1]) ;

	/* A keeps a B value; B's runtime is deleted; A sees a stale handle, and doesn't crash. */
	ugly_handle ac = make_counter(&f, f.mod) ;
	ugly_val items ;
	ugly_get_member(ctx, ac, "items", UGLY_HANDLE, &items) ;
	st = ugly_set_index(ctx, items.as.h, 0, ugly_hval(bc)) ;
	is_status(ctx, st, UGLY_OK, "A stores B's counter in one of its lists") ;
	ugly_release(ctx, bc) ;
	ugly_release(ctx, b_echo) ;
	ugly_release(ctx, b_raise) ;
	ugly_release(ctx, mb) ;
	st = ugly_rt_delete(ctx, rb) ;
	is_status(ctx, st, UGLY_OK, "delete B's runtime while A holds one of its values") ;
	st = ugly_get_index(ctx, items.as.h, 0, UGLY_ANY, &v) ;
	if (st == UGLY_OK){
		ugly_handle held = v.as.h ;
		st = ugly_invoke(ctx, held, "inc", NULL, 0, UGLY_ANY, &v) ;
		ugly_release(ctx, held) ;
	}
	is_status(ctx, st, UGLY_ERR_STALE_HANDLE, "  ... using it from A: UGLY_ERR_STALE_HANDLE") ;
	ugly_val_release(ctx, &items) ;
	ugly_release(ctx, ac) ;
	ok(finalized(&f, f.mod) >= 0, "  ... and A collects its dead proxy without trouble") ;
	st = ugly_lang_unload(ctx, lb) ;
	is_status(ctx, st, UGLY_OK, "unload B") ;

	ugly_release(ctx, a_echo) ;
	fx_teardown(&f) ;
	return done_testing() ;
}
