/* Callbacks and re-entry: C -> language -> C ... to depth 100, and across runtimes (§4). */
#include "conformance.h"


static ugly_status add1(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)data ; (void)want ;
	if ((n != 1)||(args[0].kind != UGLY_INT)){
		return ugly_raise(ctx, UGLY_ERR_TYPE, "TypeError", "add1 wants one int", UGLY_NO_HANDLE) ;
	}
	*out = ugly_int(args[0].as.i + 1) ;
	return UGLY_OK ;
}


/* recurse(n) = n == 0 ? 0 : 1 + apply(recurse, n - 1), with apply in the language under test. */
typedef struct { fixture *f ; ugly_handle self ; int deepest ; } rec ;
static int depth ;

static ugly_status recurse(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)want ;
	rec *r = data ;
	if ((n != 1)||(args[0].kind != UGLY_INT)){
		return ugly_raise(ctx, UGLY_ERR_TYPE, "TypeError", "recurse wants one int", UGLY_NO_HANDLE) ;
	}
	if (++depth > r->deepest){
		r->deepest = depth ;
	}
	ugly_status st = UGLY_OK ;
	if (args[0].as.i == 0){
		*out = ugly_int(0) ;
	}
	else {
		ugly_val a[] = { ugly_hval(r->self), ugly_int(args[0].as.i - 1) } ;
		ugly_val v ;
		st = fcall(r->f, r->f->mod, "apply", a, 2, UGLY_INT, &v) ;
		if (st == UGLY_OK){
			*out = ugly_int(v.as.i + 1) ;
		}
	}
	depth-- ;
	return st ;
}


/* glue(x) = rt2.apply(rt1.echo, x): lets rt1 reach rt2 and come back to rt1. */
typedef struct { fixture *f ; ugly_handle apply2 ; ugly_handle echo1 ; } glue_data ;

static ugly_status glue(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	glue_data *g = data ;
	if (n != 1){
		return ugly_raise(ctx, UGLY_ERR_TYPE, "TypeError", "glue wants one argument", UGLY_NO_HANDLE) ;
	}
	ugly_val a[] = { ugly_hval(g->echo1), args[0] } ;
	return ugly_call(ctx, g->apply2, a, 2, want, out) ;
}


int main(void){
	fixture f ;
	fx_setup(&f) ;
	ugly_ctx *ctx = f.ctx ;
	ugly_val v ;
	ugly_status st ;

	ugly_handle h_add1 ;
	ugly_callable_new(ctx, add1, NULL, NULL, &h_add1) ;
	ugly_val a[] = { ugly_hval(h_add1), ugly_int(41) } ;
	st = fcall(&f, f.mod, "apply", a, 2, UGLY_INT, &v) ;
	is_status(ctx, st, UGLY_OK, "apply(add1, 41): the language calls a C callback") ;
	is_int(v.as.i, 42, "  ... returns 42") ;
	a[1] = ugly_int(1) ;
	st = fcall(&f, f.mod, "call_twice", a, 2, UGLY_INT, &v) ;
	is_status(ctx, st, UGLY_OK, "call_twice(add1, 1)") ;
	is_int(v.as.i, 3, "  ... returns 3") ;

	/* A C callable is an ordinary callable handle, from C too. */
	ugly_val x = ugly_int(9) ;
	st = ugly_call(ctx, h_add1, &x, 1, UGLY_INT, &v) ;
	ok((st == UGLY_OK)&&(v.as.i == 10), "ugly_call on a C callable") ;
	uint32_t traits = 0 ;
	ugly_val tn ;
	ugly_describe(ctx, h_add1, &traits, &tn) ;
	ok((traits & UGLY_TRAIT_CALLABLE) != 0, "  ... described as CALLABLE") ;
	ugly_val_release(ctx, &tn) ;
	ugly_release(ctx, h_add1) ;

	/* Re-entry: C -> language -> C -> language ... 100 levels. */
	rec r = { &f, UGLY_NO_HANDLE, 0 } ;
	ugly_callable_new(ctx, recurse, &r, NULL, &r.self) ;
	x = ugly_int(100) ;
	st = ugly_call(ctx, r.self, &x, 1, UGLY_INT, &v) ;
	is_status(ctx, st, UGLY_OK, "recursion through the language, 100 levels deep") ;
	is_int(v.as.i, 100, "  ... returns 100") ;
	is_int(r.deepest, 101, "  ... reached depth 101") ;
	ugly_release(ctx, r.self) ;

	/* Across runtimes: rt1 -> C -> rt2 -> rt1. */
	ugly_rt *rt2 ;
	ugly_handle mod2 ;
	if (fx_runtime(&f, &rt2, &mod2)){
		glue_data g = { &f, UGLY_NO_HANDLE, UGLY_NO_HANDLE } ;
		ugly_val t ;
		ugly_get_member(ctx, mod2, "apply", UGLY_HANDLE, &t) ;
		g.apply2 = t.as.h ;
		ugly_get_member(ctx, f.mod, "echo", UGLY_HANDLE, &t) ;
		g.echo1 = t.as.h ;
		ugly_handle h_glue ;
		ugly_callable_new(ctx, glue, &g, NULL, &h_glue) ;
		ugly_val b[] = { ugly_hval(h_glue), ugly_str("round trip", 10) } ;
		st = fcall(&f, f.mod, "apply", b, 2, UGLY_STRING, &v) ;
		is_status(ctx, st, UGLY_OK, "rt1.apply(glue, s): rt1 -> C -> rt2 -> rt1") ;
		is_str(sval(&v), "round trip", "  ... returns s") ;
		ugly_val_release(ctx, &v) ;
		ugly_release(ctx, h_glue) ;
		ugly_release(ctx, g.apply2) ;
		ugly_release(ctx, g.echo1) ;
		ugly_release(ctx, mod2) ;
		st = ugly_rt_delete(ctx, rt2) ;
		is_status(ctx, st, UGLY_OK, "second runtime deleted") ;
	}

	fx_teardown(&f) ;
	return done_testing() ;
}
