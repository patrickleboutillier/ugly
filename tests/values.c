/* Values: every kind round-trips through echo; 'want' follows the home language's rules (§1). */
#include <stdint.h>
#include "conformance.h"


static void roundtrip(fixture *f, ugly_val in, const char *what){
	ugly_val out ;
	ugly_status st = fcall(f, f->mod, "echo", &in, 1, UGLY_ANY, &out) ;
	if (! is_status(f->ctx, st, UGLY_OK, what)){
		return ;
	}
	int same = (out.kind == in.kind) ;
	if (same){
		switch (in.kind){
			case UGLY_BOOL: same = (out.as.b == in.as.b) ; break ;
			case UGLY_INT: same = (out.as.i == in.as.i) ; break ;
			case UGLY_DOUBLE: same = (isnan(in.as.d) ? isnan(out.as.d) : out.as.d == in.as.d) ; break ;
			case UGLY_STRING: same = (out.as.str.len == in.as.str.len)
				&&(memcmp(out.as.str.ptr, in.as.str.ptr, in.as.str.len) == 0) ; break ;
			case UGLY_BYTES: same = (out.as.bytes.len == in.as.bytes.len)
				&&(memcmp(out.as.bytes.ptr, in.as.bytes.ptr, in.as.bytes.len) == 0) ; break ;
			default: break ;
		}
	}
	ok(same, "%s: same kind and value back", what) ;
	ugly_val_release(f->ctx, &out) ;
}


static void want(fixture *f, ugly_val in, ugly_kind k, ugly_status expect, const char *what, ugly_val *out){
	ugly_status st = fcall(f, f->mod, "echo", &in, 1, k, out) ;
	is_status(f->ctx, st, expect, what) ;
}


static void kind_of(fixture *f, ugly_val in, const char *expect, const char *what){
	ugly_val out ;
	ugly_status st = fcall(f, f->mod, "kind_of", &in, 1, UGLY_STRING, &out) ;
	if (is_status(f->ctx, st, UGLY_OK, what)){
		is_str(sval(&out), expect, what) ;
		ugly_val_release(f->ctx, &out) ;
	}
}


int main(void){
	fixture f ;
	fx_setup(&f) ;

	roundtrip(&f, ugly_null(), "null") ;
	roundtrip(&f, ugly_bool(true), "bool true") ;
	roundtrip(&f, ugly_bool(false), "bool false") ;
	roundtrip(&f, ugly_int(0), "int 0") ;
	roundtrip(&f, ugly_int(INT64_MAX), "int64 max") ;
	roundtrip(&f, ugly_int(INT64_MIN), "int64 min") ;
	roundtrip(&f, ugly_double(3.5), "double") ;
	roundtrip(&f, ugly_double(NAN), "NaN") ;
	roundtrip(&f, ugly_double(INFINITY), "+inf") ;
	roundtrip(&f, ugly_double(-INFINITY), "-inf") ;
	roundtrip(&f, ugly_str("", 0), "empty string") ;
	roundtrip(&f, ugly_str("h\xc3\xa9llo \xe2\x82\xac", 10), "non-ASCII UTF-8 string") ;
	roundtrip(&f, ugly_str("a\0b", 3), "string with an embedded NUL") ;
	static const uint8_t bytes[] = { 0, 1, 127, 128, 255 } ;
	roundtrip(&f, ugly_bytes(bytes, sizeof(bytes)), "bytes, including 0 and 255") ;

	/* The caller's 'want', converted by the home plugin with its own rules (mock: Perl-like). */
	ugly_val out ;
	want(&f, ugly_str("42", 2), UGLY_INT, UGLY_OK, "want INT from \"42\": converted", &out) ;
	is_int(out.kind == UGLY_INT ? out.as.i : -1, 42, "  ... to 42") ;
	want(&f, ugly_int(7), UGLY_STRING, UGLY_OK, "want STRING from 7: converted", &out) ;
	is_str(sval(&out), "7", "  ... to \"7\"") ;
	ugly_val_release(f.ctx, &out) ;
	want(&f, ugly_double(2.0), UGLY_INT, UGLY_OK, "want INT from 2.0: converted", &out) ;
	is_int(out.kind == UGLY_INT ? out.as.i : -1, 2, "  ... to 2") ;
	want(&f, ugly_str("x", 1), UGLY_INT, UGLY_ERR_TYPE, "want INT from \"x\": UGLY_ERR_TYPE", &out) ;
	const ugly_error *e = ugly_last_error(f.ctx) ;
	is_str(e ? ugly_error_language(e) : NULL, plugin_name(), "  ... reported by the home language") ;
	want(&f, ugly_double(2.5), UGLY_INT, UGLY_ERR_TYPE, "want INT from 2.5: UGLY_ERR_TYPE", &out) ;
	ok(out.kind == UGLY_NULL, "  ... and no value") ;

	/* Inside the language, each kind arrives as its native type. */
	kind_of(&f, ugly_null(), "null", "kind_of(null)") ;
	kind_of(&f, ugly_int(1), "int", "kind_of(int)") ;
	kind_of(&f, ugly_double(1.5), "double", "kind_of(double)") ;
	kind_of(&f, ugly_str("s", 1), "string", "kind_of(string)") ;

	fx_teardown(&f) ;
	return done_testing() ;
}
