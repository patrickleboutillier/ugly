/*
 * Calls into Haskell, until the plugin joins the conformance suite: the
 * functions of the test module Smoke. Currying, the literal rules
 * for arguments (doc/languages/haskell.md), IO, Haskell functions coming home,
 * invoke with the receiver last, and overloads chosen by the arguments.
 */
#include "tap.h"

static ugly_handle mod ;

/* The member 'name' of Smoke, as a handle (owned). */
static ugly_handle fn(const char *name){
	ugly_val v = ugly_null() ;
	ugly_get_member(ctx, mod, name, UGLY_HANDLE, &v) ;
	return v.kind == UGLY_HANDLE ? v.as.h : UGLY_NO_HANDLE ;
}

/* Calls Smoke's 'name' with args, as 'want'. */
static ugly_status call(const char *name, const ugly_val *args, size_t nargs, ugly_kind want, ugly_val *out){
	ugly_handle f = fn(name) ;
	*out = ugly_null() ;
	ugly_status st = ugly_call(ctx, f, args, nargs, want, out) ;
	ugly_release(ctx, f) ;
	return st ;
}


int main(void){
	ugly_rt *rt = start("Smoke", &mod) ;
	ugly_val v, w ;
	ugly_handle h, f ;
	bool same ;

	/* Functions are callable, and curried. */
	f = fn("add") ;
	ok(type_is(f, UGLY_TRAIT_CALLABLE, "Int -> Int -> Int"), "add: CALLABLE, Int -> Int -> Int") ;
	ok((ugly_call(ctx, f, (ugly_val[]){ ugly_int(2), ugly_int(3) }, 2, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 5),
		"add 2 3 is 5") ;
	ok((ugly_call(ctx, f, (ugly_val[]){ ugly_int(2) }, 1, UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_HANDLE)
		&&type_is(v.as.h, UGLY_TRAIT_CALLABLE, "Int -> Int"), "add 2: a partial application, Int -> Int") ;
	ok((ugly_call(ctx, v.as.h, (ugly_val[]){ ugly_int(10) }, 1, UGLY_ANY, &w) == UGLY_OK)&&is_int(w, 12),
		"(add 2) 10 is 12") ;
	h = v.as.h ;
	ok(error_is(ugly_call(ctx, f, (ugly_val[]){ ugly_str("x", 1), ugly_int(1) }, 2, UGLY_ANY, &v), UGLY_ERR_TYPE,
		"TypeError", "a string isn't an Int"), "add \"x\" 1: TYPE") ;
	ok(error_is(ugly_call(ctx, f, (ugly_val[]){ ugly_int(1), ugly_int(2), ugly_int(3) }, 3, UGLY_ANY, &v),
		UGLY_ERR_TYPE, "TypeError", "too many arguments"), "add 1 2 3: TYPE, too many arguments") ;
	ok(error_is(ugly_call(ctx, f, NULL, 0, UGLY_ANY, &v), UGLY_ERR_TYPE, "TypeError", "expects an argument"),
		"add with no arguments: TYPE") ;
	ugly_release(ctx, f) ;

	/* Arguments convert as literals would. */
	ok((call("greet", (ugly_val[]){ ugly_str("Bob", 3) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_string(v, "hello, Bob"),
		"greet \"Bob\": a string is a String") ;
	ugly_val_release(ctx, &v) ;
	ok((call("halve", (ugly_val[]){ ugly_int(5) }, 1, UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_DOUBLE)&&(v.as.d == 2.5),
		"halve 5: an int is a Double, as a literal is") ;
	ok((call("widen", (ugly_val[]){ ugly_int(200) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 200),
		"widen 200: an int is a Word8 if it fits") ;
	ok(error_is(call("widen", (ugly_val[]){ ugly_int(300) }, 1, UGLY_ANY, &v), UGLY_ERR_TYPE, "TypeError",
		"300 doesn't fit in Word8"), "widen 300: TYPE, never wrapped") ;
	ok(error_is(call("add", (ugly_val[]){ ugly_double(1.0), ugly_int(1) }, 2, UGLY_ANY, &v), UGLY_ERR_TYPE,
		"TypeError", "double"), "add 1.0 1: a double isn't an Int") ;
	ok((call("safeDiv", (ugly_val[]){ ugly_int(7), ugly_int(2) }, 2, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 3),
		"safeDiv 7 2: Just 3 is 3") ;
	ok((call("safeDiv", (ugly_val[]){ ugly_int(1), ugly_int(0) }, 2, UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_NULL),
		"safeDiv 1 0: Nothing is null") ;
	ok((call("orZero", (ugly_val[]){ ugly_null() }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 0),
		"orZero null: null is Nothing") ;
	ok((call("orZero", (ugly_val[]){ ugly_int(5) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 5),
		"orZero 5: 5 is Just 5") ;

	/* Values come home as themselves. */
	ok((call("identity", (ugly_val[]){ ugly_int(7) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 7),
		"identity 7: Value takes anything") ;
	f = fn("color") ;
	ok((call("identity", (ugly_val[]){ ugly_hval(f) }, 1, UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_HANDLE)
		&&(ugly_same(ctx, f, v.as.h, &same) == UGLY_OK)&&same, "identity color: the same handle back") ;
	ugly_val_release(ctx, &v) ;
	ok((call("apply", (ugly_val[]){ ugly_hval(h), ugly_int(40) }, 2, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 42),
		"apply (add 2) 40: a Haskell function comes home as a function") ;
	ugly_release(ctx, h) ;
	h = fn("greet") ;
	ok(error_is(call("apply", (ugly_val[]){ ugly_hval(h), ugly_int(1) }, 2, UGLY_ANY, &v), UGLY_ERR_TYPE,
		"TypeError", "isn't an Int -> Int"), "apply greet 1: a String -> String isn't an Int -> Int") ;
	ugly_release(ctx, h) ;
	ugly_handle list ;
	ugly_list_new(ctx, NULL, 0, &list) ;
	same = false ;
	ok((call("identity", (ugly_val[]){ ugly_hval(list) }, 1, UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_HANDLE)
		&&(ugly_same(ctx, v.as.h, list, &same) == UGLY_OK)&&same, "identity of a foreign value: itself (haskell-foreign has more)") ;
	ugly_val_release(ctx, &v) ;
	ugly_release(ctx, list) ;

	/* Exceptions raise from the call. */
	ok(error_is(call("explode", (ugly_val[]){ ugly_int(-1) }, 1, UGLY_ANY, &v), UGLY_ERR_RAISED, "ErrorCall",
		"not positive"), "explode (-1): RAISED ErrorCall") ;
	ok((call("explode", (ugly_val[]){ ugly_int(3) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 3), "explode 3 is 3") ;

	/* IO actions run when called. */
	h = fn("tick") ;
	ok(type_is(h, UGLY_TRAIT_CALLABLE, "IO Int"), "tick: CALLABLE, IO Int") ;
	ok((ugly_call(ctx, h, NULL, 0, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 1)
		&&(ugly_call(ctx, h, NULL, 0, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 2), "tick (), tick (): 1, then 2") ;
	ugly_release(ctx, h) ;
	ok((call("tickBy", (ugly_val[]){ ugly_int(10) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 12),
		"tickBy 10: a function's IO result runs: 12") ;

	/* invoke: a module's function, or a method with the receiver last. */
	ok((ugly_invoke(ctx, mod, "add", (ugly_val[]){ ugly_int(1), ugly_int(2) }, 2, UGLY_ANY, &v) == UGLY_OK)
		&&is_int(v, 3), "Smoke.add(1, 2): no receiver") ;
	ok((ugly_invoke(ctx, f, "next", NULL, 0, UGLY_STRING, &v) == UGLY_OK)&&is_string(v, "Blue"),
		"color.next(): next Green is Blue") ;
	ugly_val_release(ctx, &v) ;
	h = fn("colors") ;
	ugly_get_index(ctx, h, 0, UGLY_ANY, &w) ;
	ok((ugly_invoke(ctx, f, "mix", &w, 1, UGLY_ANY, &v) == UGLY_OK)&&is_string(v, "Red+Green"),
		"color.mix(red): mix Red Green, the receiver last") ;
	ugly_val_release(ctx, &v) ;
	ugly_val_release(ctx, &w) ;
	ugly_release(ctx, h) ;
	ok(error_is(ugly_invoke(ctx, f, "nope", NULL, 0, UGLY_ANY, &v), UGLY_ERR_NOT_FOUND, "NotInScope", "nope"),
		"color.nope(): NOT_FOUND") ;
	ok(error_is(ugly_invoke(ctx, f, "answer", NULL, 0, UGLY_ANY, &v), UGLY_ERR_NOT_CAPABLE, "TypeError",
		"isn't a function"), "color.answer(): NOT_CAPABLE") ;

	/* Overloads: the instantiation the arguments fit best. */
	h = fn("size") ;
	ok(type_is(h, UGLY_TRAIT_CALLABLE, "size :: Sized a => a -> Int"), "size: one callable, its polymorphic type") ;
	ugly_release(ctx, h) ;
	h = fn("primes") ;
	ok((call("size", (ugly_val[]){ ugly_hval(h) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 5),
		"size primes: the [Int] instance, 5") ;
	ugly_release(ctx, h) ;
	ok((call("size", (ugly_val[]){ ugly_str("abc", 3) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 3),
		"size \"abc\": the String instance, 3") ;
	ok((call("size", (ugly_val[]){ ugly_hval(f) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_int(v, 1),
		"size color: the Color instance, 1") ;
	ok(error_is(call("size", (ugly_val[]){ ugly_int(1) }, 1, UGLY_ANY, &v), UGLY_ERR_TYPE, "TypeError",
		"no instance of size"), "size 1: TYPE, no instance") ;
	ok((call("narrow", (ugly_val[]){ ugly_int(-1) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_string(v, "Int8 -1"),
		"narrow (-1): only Int8 fits") ;
	ugly_val_release(ctx, &v) ;
	ok((call("narrow", (ugly_val[]){ ugly_int(200) }, 1, UGLY_ANY, &v) == UGLY_OK)&&is_string(v, "Word8 200"),
		"narrow 200: only Word8 fits") ;
	ugly_val_release(ctx, &v) ;
	ok(error_is(call("narrow", (ugly_val[]){ ugly_int(5) }, 1, UGLY_ANY, &v), UGLY_ERR_TYPE, "TypeError",
		"ambiguous"), "narrow 5: TYPE, ambiguous") ;
	ugly_release(ctx, f) ;

	ugly_release(ctx, mod) ;
	ok(ugly_rt_delete(ctx, rt) == UGLY_OK, "runtime deleted") ;
	all_released() ;
	ugly_ctx_delete(ctx) ;
	printf("1..%d\n", n) ;
	return failed ;
}
