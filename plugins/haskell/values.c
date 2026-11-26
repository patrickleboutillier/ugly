/*
 * The Haskell plugin's values and boxes, until it joins the conformance
 * suite: every kind of value in the test module Smoke, read back
 * with each 'want', by Haskell's rules (doc/haskell-bridge.md): primitives
 * by value, everything else boxed, read-only, lazy where Haskell is, and
 * exceptions raised where a value is forced.
 */
#include "tap.h"

static ugly_handle mod ;

/* Member 'name' of Smoke, as 'want'; the status. */
static ugly_status get(const char *name, ugly_kind want, ugly_val *out){
	*out = ugly_null() ;
	return ugly_get_member(ctx, mod, name, want, out) ;
}


int main(void){
	ugly_lang *lang = NULL ;
	ugly_rt *rt = NULL ;
	ugly_val v, w ;
	ugly_handle h, h2 ;
	int64_t len ;
	bool same ;

	setvbuf(stdout, NULL, _IOLBF, 0) ;
	ugly_ctx_new(&ctx) ;
	if ((ugly_lang_load(ctx, "haskell", &lang) != UGLY_OK)||(ugly_rt_new(ctx, lang, &rt) != UGLY_OK)){
		printf("not ok 1 - haskell starts\n1..1\n") ;
		return 1 ;
	}

	/* Modules. */
	ok(ugly_rt_import(ctx, rt, "Smoke", &mod) == UGLY_OK, "import Smoke (from Smoke.so, on UGLY_HASKELL_PATH)") ;
	ok(type_is(mod, UGLY_TRAIT_MEMBERS, "module"), "a module: MEMBERS, type module") ;
	ok((ugly_rt_import(ctx, rt, "Smoke", &h) == UGLY_OK)&&(ugly_same(ctx, mod, h, &same) == UGLY_OK)&&same,
		"imported again: the same handle") ;
	ugly_release(ctx, h) ;
	ok(error_is(ugly_rt_import(ctx, rt, "No.Such", &h), UGLY_ERR_NOT_FOUND, "ModuleNotFound", "No.Such"),
		"import No.Such: NOT_FOUND, ModuleNotFound") ;
	ok((ugly_member_names(ctx, mod, &h) == UGLY_OK)&&(ugly_length(ctx, h, &len) == UGLY_OK)&&(len == 49),
		"member_names: the 49 exports") ;
	ok((ugly_get_index(ctx, h, 2, UGLY_ANY, &v) == UGLY_OK)&&is_string(v, "answer"), "in order: answer is third") ;
	ugly_val_release(ctx, &v) ;
	ugly_release(ctx, h) ;
	ok(error_is(get("nope", UGLY_ANY, &v), UGLY_ERR_NOT_FOUND, "NotInScope", "nope"), "no such member: NotInScope") ;

	/* Primitives, and 'want' by Haskell's rules: strict, but an int widens to a double. */
	ok((get("unit", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_NULL), "() is null") ;
	ok((get("yes", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_BOOL)&&v.as.b, "True is true") ;
	ok((get("answer", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_INT)&&(v.as.i == 42), "an Int is an int") ;
	ok((get("answer", UGLY_DOUBLE, &v) == UGLY_OK)&&(v.kind == UGLY_DOUBLE)&&(v.as.d == 42.0), "want DOUBLE: widened") ;
	ok(error_is(get("answer", UGLY_STRING, &v), UGLY_ERR_TYPE, "TypeError", "string"), "want STRING from an Int: TYPE") ;
	ok(error_is(get("answer", UGLY_HANDLE, &v), UGLY_ERR_TYPE, "TypeError", "handle"), "want HANDLE from an Int: TYPE") ;
	ok((get("byte", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_INT)&&(v.as.i == 200), "a Word8 is an int") ;
	ok((get("half", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_DOUBLE)&&(v.as.d == 0.5), "a Double is a double") ;
	ok(error_is(get("half", UGLY_INT, &v), UGLY_ERR_TYPE, "TypeError", "int"), "want INT from 0.5: TYPE") ;
	ok((get("greeting", UGLY_ANY, &v) == UGLY_OK)&&is_string(v, "h\xc3\xa9llo"), "a String is UTF-8") ;
	ugly_val_release(ctx, &v) ;
	ok((get("bytes", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_BYTES)&&(v.as.bytes.len == 2)
		&&(v.as.bytes.ptr[0] == 0)&&(v.as.bytes.ptr[1] == 255), "a ByteString is bytes") ;
	ugly_val_release(ctx, &v) ;
	ok(error_is(get("bytes", UGLY_STRING, &v), UGLY_ERR_TYPE, "TypeError", "string"), "want STRING from bytes: TYPE") ;

	/* An Integer beyond int64 doesn't wrap: it's a box of its digits. */
	ok((get("big", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_HANDLE)&&type_is(v.as.h, 0, "Integer"),
		"2^70 is a box: Integer") ;
	ugly_val_release(ctx, &v) ;
	ok((get("big", UGLY_STRING, &v) == UGLY_OK)&&is_string(v, "1180591620717411303424"), "its text is its digits") ;
	ugly_val_release(ctx, &v) ;
	ok(error_is(get("big", UGLY_INT, &v), UGLY_ERR_TYPE, "TypeError", "Integer"), "want INT from it: TYPE") ;

	/* A lone surrogate isn't text: a box with no string. */
	ok((get("surrogate", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_HANDLE), "a String with a lone surrogate is a box") ;
	ugly_val_release(ctx, &v) ;
	ok(get("surrogate", UGLY_STRING, &v) == UGLY_ERR_TYPE, "want STRING from it: TYPE") ;

	/* Boxes: a type, a text (show), identity. */
	ok((get("color", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_HANDLE)&&type_is(v.as.h, 0, "Color"),
		"a Color is a box: no traits, type Color") ;
	ok((get("color", UGLY_STRING, &w) == UGLY_OK)&&is_string(w, "Green"), "want STRING: its show") ;
	ugly_val_release(ctx, &w) ;
	ok((get("color", UGLY_HANDLE, &w) == UGLY_OK)&&(ugly_same(ctx, v.as.h, w.as.h, &same) == UGLY_OK)&&same,
		"the same value twice: the same handle") ;
	ugly_val_release(ctx, &w) ;
	ok(error_is(ugly_get_member(ctx, v.as.h, "x", UGLY_ANY, &w), UGLY_ERR_NOT_CAPABLE, "TypeError", "no members"),
		"a box without members: NOT_CAPABLE") ;
	ugly_val_release(ctx, &v) ;

	/* Lists: read-only elements, lazy. */
	get("primes", UGLY_ANY, &v) ;
	h = v.as.h ;
	ok((v.kind == UGLY_HANDLE)&&type_is(h, UGLY_TRAIT_ELEMENTS, "[Int]"), "[Int]: ELEMENTS") ;
	ok((ugly_length(ctx, h, &len) == UGLY_OK)&&(len == 5), "length 5") ;
	ok((ugly_get_index(ctx, h, 4, UGLY_ANY, &w) == UGLY_OK)&&(w.kind == UGLY_INT)&&(w.as.i == 11), "[4] is 11") ;
	ok(error_is(ugly_get_index(ctx, h, 5, UGLY_ANY, &w), UGLY_ERR_NOT_FOUND, "ArrayException", "range"),
		"[5]: NOT_FOUND") ;
	ok(ugly_set_index(ctx, h, 0, ugly_int(1)) == UGLY_ERR_NOT_CAPABLE, "set_index: NOT_CAPABLE (immutable)") ;
	ok((ugly_get_member(ctx, mod, "primes", UGLY_STRING, &w) == UGLY_OK)&&is_string(w, "[2,3,5,7,11]"),
		"want STRING: [2,3,5,7,11]") ;
	ugly_val_release(ctx, &w) ;
	ugly_release(ctx, h) ;
	get("colors", UGLY_ANY, &v) ;
	ok((ugly_get_index(ctx, v.as.h, 2, UGLY_STRING, &w) == UGLY_OK)&&is_string(w, "Blue"), "[Color]: [2] is Blue") ;
	ugly_val_release(ctx, &w) ;
	ugly_val_release(ctx, &v) ;
	get("naturals", UGLY_ANY, &v) ;
	ok((ugly_get_index(ctx, v.as.h, 1000, UGLY_ANY, &w) == UGLY_OK)&&(w.as.i == 1000),
		"[0..] is infinite, and [1000] is 1000") ;
	ugly_val_release(ctx, &v) ;

	/* Maybe: Nothing is null, Just x is x, Just Nothing stays a box. */
	ok((get("nothing", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_NULL), "Nothing is null") ;
	ok((get("just", UGLY_ANY, &v) == UGLY_OK)&&is_string(v, "x"), "Just 'x' is \"x\"") ;
	ugly_val_release(ctx, &v) ;
	ok((get("justNothing", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_HANDLE)
		&&type_is(v.as.h, UGLY_TRAIT_ELEMENTS, "Maybe (Maybe Int)"), "Just Nothing is a box, one element long") ;
	ok((ugly_get_index(ctx, v.as.h, 0, UGLY_ANY, &w) == UGLY_OK)&&(w.kind == UGLY_NULL), "whose element is null") ;
	ugly_val_release(ctx, &v) ;

	/* Tuples and maps. */
	get("pair", UGLY_ANY, &v) ;
	ok(type_is(v.as.h, UGLY_TRAIT_ELEMENTS, "(Int,[Char])"), "a pair: ELEMENTS") ;
	ok((ugly_get_index(ctx, v.as.h, 1, UGLY_ANY, &w) == UGLY_OK)&&is_string(w, "one"), "[1] is \"one\"") ;
	ugly_val_release(ctx, &w) ;
	ugly_val_release(ctx, &v) ;
	ok((get("pair", UGLY_STRING, &v) == UGLY_OK)&&is_string(v, "(1,\"one\")"), "its text: (1,\"one\")") ;
	ugly_val_release(ctx, &v) ;
	get("table", UGLY_ANY, &v) ;
	ok(type_is(v.as.h, UGLY_TRAIT_MEMBERS, "Map [Char] Int"), "a Map String Int: MEMBERS") ;
	ok((ugly_get_member(ctx, v.as.h, "b", UGLY_ANY, &w) == UGLY_OK)&&(w.as.i == 2), "its b is 2") ;
	ok(ugly_get_member(ctx, v.as.h, "c", UGLY_ANY, &w) == UGLY_ERR_NOT_FOUND, "no c: NOT_FOUND") ;
	ok(ugly_set_member(ctx, v.as.h, "c", ugly_int(3)) == UGLY_ERR_NOT_CAPABLE, "set_member: NOT_CAPABLE") ;
	ugly_val_release(ctx, &v) ;

	/* Exceptions are raised where a value is forced, never later. */
	ok(error_is(get("boom", UGLY_ANY, &v), UGLY_ERR_RAISED, "ErrorCall", "boom"), "error \"boom\": RAISED ErrorCall") ;
	h2 = UGLY_NO_HANDLE ;
	ok((ugly_error_payload(ctx, ugly_last_error(ctx), &h2) == UGLY_OK)&&type_is(h2, 0, "ErrorCall"),
		"its payload: a box of the exception") ;
	ugly_release(ctx, h2) ;
	ok(error_is(get("divZero", UGLY_ANY, &v), UGLY_ERR_RAISED, "ArithException", "divide by zero"),
		"1 `div` 0: RAISED ArithException") ;
	ok((get("lazyBoom", UGLY_ANY, &v) == UGLY_OK)&&(v.kind == UGLY_HANDLE), "[1, error .., 3]: a list, unforced") ;
	ok((ugly_get_index(ctx, v.as.h, 2, UGLY_ANY, &w) == UGLY_OK)&&(w.as.i == 3), "[2] is 3") ;
	ok(error_is(ugly_get_index(ctx, v.as.h, 1, UGLY_ANY, &w), UGLY_ERR_RAISED, "ErrorCall", "lazy boom"),
		"[1] raises, when forced") ;
	ugly_val_release(ctx, &v) ;

	ugly_release(ctx, mod) ;
	ok(ugly_rt_delete(ctx, rt) == UGLY_OK, "runtime deleted") ;
	/* Every ref the plugin made was released: the Haskell side counts them (libugly_hs.so is global). */
	all_released() ;
	ugly_ctx_delete(ctx) ;
	printf("1..%d\n", n) ;
	return failed ;
}
