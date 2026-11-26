/*
 * Foreign values in Haskell, until the plugin joins the conformance suite:
 * values whose home is the C host (callables, a core list and map) passed
 * to the test module Smoke. A foreign callable as a Haskell function, pure
 * or in IO; foreign values kept, used later, and released when Haskell
 * drops them; operations on them from Haskell (Ugly); errors both ways.
 */
#include "tap.h"

static ugly_handle mod ;

/* Calls Smoke's 'name' with args, as 'want'. */
static ugly_status call(const char *name, const ugly_val *args, size_t nargs, ugly_kind want, ugly_val *out){
	*out = ugly_null() ;
	return ugly_invoke(ctx, mod, name, args, nargs, want, out) ;
}

/* C callables. */
static ugly_status inc(ugly_ctx *c, void *data, const ugly_val *args, size_t nargs, ugly_kind want, ugly_val *out){
	(void)data ; (void)want ;
	if ((nargs != 1)||(args[0].kind != UGLY_INT)){
		return ugly_raise(c, UGLY_ERR_TYPE, "Usage", "inc(int)", UGLY_NO_HANDLE) ;
	}
	*out = ugly_int(args[0].as.i + 1) ;
	return UGLY_OK ;
}

static ugly_status sum(ugly_ctx *c, void *data, const ugly_val *args, size_t nargs, ugly_kind want, ugly_val *out){
	(void)data ; (void)want ;
	if ((nargs != 2)||(args[0].kind != UGLY_INT)||(args[1].kind != UGLY_INT)){
		return ugly_raise(c, UGLY_ERR_TYPE, "Usage", "sum(int, int)", UGLY_NO_HANDLE) ;
	}
	*out = ugly_int(args[0].as.i + args[1].as.i) ;
	return UGLY_OK ;
}

static ugly_status boom(ugly_ctx *c, void *data, const ugly_val *args, size_t nargs, ugly_kind want, ugly_val *out){
	(void)data ; (void)args ; (void)nargs ; (void)want ; (void)out ;
	return ugly_raise(c, UGLY_ERR_RAISED, "Boom", "boom from C", UGLY_NO_HANDLE) ;
}

/* Calls back into Haskell: Smoke's function named by data, with the argument. */
static ugly_status back(ugly_ctx *c, void *data, const ugly_val *args, size_t nargs, ugly_kind want, ugly_val *out){
	return ugly_invoke(c, mod, data, args, nargs, want, out) ;
}

static ugly_handle callable(ugly_c_fn fn, void *data){
	ugly_handle h = UGLY_NO_HANDLE ;
	ugly_callable_new(ctx, fn, data, NULL, &h) ;
	return h ;
}

/* The current error: this status, language, type, and message containing this (NULL: any). */
static int error_from(ugly_status st, ugly_status expect, const char *language, const char *type, const char *message){
	const ugly_error *e = ugly_last_error(ctx) ;
	int good = (st == expect)&&(e != NULL)&&((language == NULL)||(strcmp(ugly_error_language(e), language) == 0))
		&&((type == NULL)||(strcmp(ugly_error_type(e), type) == 0))&&(strstr(ugly_error_message(e), message) != NULL) ;
	if (! good){
		printf("# status %d, error %s %s: %s\n", st, e ? ugly_error_language(e) : "-", e ? ugly_error_type(e) : "-",
			e ? ugly_error_message(e) : "-") ;
	}
	return good ;
}

static int string_is(ugly_status st, ugly_val *v, const char *s){
	int good = (st == UGLY_OK)&&is_string(*v, s) ;
	if (! good){
		printf("# status %d: %.*s\n", st, v->kind == UGLY_STRING ? (int)v->as.str.len : 0,
			v->kind == UGLY_STRING ? v->as.str.ptr : "") ;
	}
	ugly_val_release(ctx, v) ;
	return good ;
}


int main(void){
	ugly_rt *rt = start("Smoke", &mod) ;
	ugly_val v ;
	bool same = false ;
	ugly_handle f_inc = callable(inc, NULL), f_sum = callable(sum, NULL), f_boom = callable(boom, NULL) ;
	ugly_handle f_add = callable(back, "add"), f_explode = callable(back, "explode") ;

	/* A foreign callable where a Haskell function is expected. */
	ok((call("apply", (ugly_val[]){ ugly_hval(f_inc), ugly_int(41) }, 2, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 42),
		"apply inc 41: a C callable as an Int -> Int, 42") ;
	ok((call("twice", (ugly_val[]){ ugly_hval(f_inc), ugly_int(40) }, 2, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 42),
		"twice inc 40: called twice, 42") ;
	ok((call("both", (ugly_val[]){ ugly_hval(f_sum) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 7),
		"both sum: an Int -> Int -> Int, one call with both arguments, 7") ;
	ok((call("applyIO", (ugly_val[]){ ugly_hval(f_inc), ugly_int(1) }, 2, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 2),
		"applyIO inc 1: an Int -> IO Int, 2") ;
	ok(error_from(call("apply", (ugly_val[]){ ugly_hval(f_add), ugly_int(1) }, 2, UGLY_ANY, &v), UGLY_ERR_RAISED,
		"haskell", "TypeError", "isn't an Int"),
		"apply with a callable returning add(1), a function: a TypeError, raised where the result is forced") ;

	/* Kept past the call: a lazy list of calls, forced later. */
	ugly_handle mapped ;
	ok((call("mapped", (ugly_val[]){ ugly_hval(f_inc) }, 1, UGLY_HANDLE, &v) == UGLY_OK)&&(v.kind == UGLY_HANDLE),
		"mapped inc: a lazy [Int]") ;
	mapped = v.as.h ;
	ugly_release(ctx, f_inc) ;
	f_inc = UGLY_NO_HANDLE ;
	ok((ugly_get_index(ctx, mapped, 2, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 4),
		"its element 2, forced after the call and after the host released inc: 4 (Haskell kept its own)") ;
	ugly_release(ctx, mapped) ;

	/* Foreign values as values: itself out again, a sequence, operations from Haskell. */
	ugly_handle list, map ;
	ugly_list_new(ctx, (ugly_val[]){ ugly_int(10), ugly_str("x", 1), ugly_null() }, 3, &list) ;
	ugly_map_new(ctx, &map) ;
	ugly_set_member(ctx, map, "k", ugly_int(5)) ;
	ok((call("identity", (ugly_val[]){ ugly_hval(list) }, 1, UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_HANDLE)
		&&(ugly_same(ctx, v.as.h, list, &same) == UGLY_OK)&&same, "identity list: the same handle (rule 5)") ;
	ugly_val_release(ctx, &v) ;
	ok((call("count", (ugly_val[]){ ugly_hval(list) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 3),
		"count list: a foreign sequence as [Value], 3") ;
	ugly_handle ints ;
	ugly_list_new(ctx, (ugly_val[]){ ugly_int(1), ugly_int(2), ugly_int(3) }, 3, &ints) ;
	ok((call("total", (ugly_val[]){ ugly_hval(ints) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 6),
		"total [1, 2, 3]: a foreign sequence as [Int], element by element, 6") ;
	ugly_release(ctx, ints) ;
	ok(error_from(call("total", (ugly_val[]){ ugly_hval(list) }, 1, UGLY_ANY, &v), UGLY_ERR_RAISED, "haskell", "TypeError",
		"isn't an Int"), "total [10, \"x\", null]: an element that isn't an Int, a TypeError when the call runs") ;
	ok((call("sizeOf", (ugly_val[]){ ugly_hval(list) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 3),
		"Ugly.size list: 3") ;
	ok((call("fieldOf", (ugly_val[]){ ugly_hval(map), ugly_str("k", 1) }, 2, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 5),
		"Ugly.get map \"k\": 5") ;
	ok(string_is(call("typeOf", (ugly_val[]){ ugly_hval(map) }, 1, UGLY_ANY, &v), &v, "map"), "Ugly.typeOf map: map") ;
	ugly_val w = ugly_null() ;
	ok((call("membersOf", (ugly_val[]){ ugly_hval(map) }, 1, UGLY_HANDLE, &v) == UGLY_OK)
		&&string_is(ugly_get_index(ctx, v.as.h, 0, UGLY_ANY, &w), &w, "k"), "Ugly.members map: [\"k\"]") ;
	ugly_val_release(ctx, &v) ;
	ok((call("callWith", (ugly_val[]){ ugly_hval(f_sum), ugly_int(1) }, 2, UGLY_ANY, &v) == UGLY_ERR_TYPE)
		&&error_from(UGLY_ERR_TYPE, UGLY_ERR_TYPE, "c", "Usage", "sum(int, int)"),
		"Ugly.call sum [1]: sum's own error, unchanged") ;

	/* Foreign errors: unchanged when they escape, a ForeignError when caught. */
	ok(error_from(call("apply", (ugly_val[]){ ugly_hval(f_boom), ugly_int(1) }, 2, UGLY_ANY, &v), UGLY_ERR_RAISED,
		"c", "Boom", "boom from C"), "apply boom 1: C's error goes through Haskell unchanged") ;
	ok(error_from(call("fieldOf", (ugly_val[]){ ugly_hval(map), ugly_str("nope", 4) }, 2, UGLY_ANY, &v),
		UGLY_ERR_NOT_FOUND, NULL, NULL, "nope"), "Ugly.get map \"nope\": NOT_FOUND, unchanged") ;
	ok(string_is(call("caughtIO", (ugly_val[]){ ugly_hval(f_boom) }, 1, UGLY_ANY, &v), &v,
		"ForeignError c Boom: boom from C"), "caught in Haskell (IO): a ForeignError") ;
	ok(string_is(call("caughtPure", (ugly_val[]){ ugly_hval(f_boom) }, 1, UGLY_ANY, &v), &v,
		"ForeignError c Boom: boom from C"), "caught in Haskell (pure, where it's forced): a ForeignError") ;

	/* A Haskell exception through a foreign callable comes home as itself (rule 7). */
	ok(string_is(call("caughtPure", (ugly_val[]){ ugly_hval(f_explode) }, 1, UGLY_ANY, &v), &v,
		"ErrorCall not positive"), "explode through C, caught in Haskell: the original ErrorCall") ;
	ok(error_from(call("apply", (ugly_val[]){ ugly_hval(f_explode), ugly_int(0) }, 2, UGLY_ANY, &v), UGLY_ERR_RAISED,
		"haskell", "ErrorCall", "not positive"), "and escaping: Haskell's ErrorCall") ;
	ok((call("apply", (ugly_val[]){ ugly_hval(f_explode), ugly_int(5) }, 2, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 5),
		"apply with a callable calling back explode(5): re-entry, 5") ;

	ugly_release(ctx, list) ;
	ugly_release(ctx, map) ;
	ugly_release(ctx, f_sum) ;
	ugly_release(ctx, f_boom) ;
	ugly_release(ctx, f_add) ;
	ugly_release(ctx, f_explode) ;

	/* Haskell releases what it dropped: foreign values, and the errors it caught. */
	ok(call("settle", NULL, 0, UGLY_ANY, &v) == UGLY_OK, "settle (GC, finalizers)") ;
	int (*live_foreign)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "ugly_hs_live_foreign") ;
	ok((live_foreign != NULL)&&(live_foreign() == 0), "no foreign value or error left in Haskell") ;
	if (live_foreign != NULL){
		printf("# %d live\n", live_foreign()) ;
	}

	ugly_release(ctx, mod) ;
	ok(ugly_rt_delete(ctx, rt) == UGLY_OK, "runtime deleted") ;
	all_released() ;
	ugly_ctx_delete(ctx) ;
	printf("1..%d\n", n) ;
	return failed ;
}
