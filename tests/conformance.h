/*
 * The v2 conformance suite: a small TAP producer plus the fixture every test
 * uses. Each test runs against the plugin named by UGLY_TEST_PLUGIN (default
 * "mock"), whose "conformance" module provides the fixtures of contract.md
 * section 9.
 */
#ifndef UGLY_CONFORMANCE_H
#define UGLY_CONFORMANCE_H

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ugly.h"

#define T_FN static __attribute__((unused))


/* ---------------------------------------------------------------- */
/* TAP                                                              */
/* ---------------------------------------------------------------- */

static int t_count, t_failed ;

T_FN void diag(const char *fmt, ...){
	va_list ap ;
	va_start(ap, fmt) ;
	printf("# ") ;
	vprintf(fmt, ap) ;
	printf("\n") ;
	va_end(ap) ;
	fflush(stdout) ;
}


T_FN int vok(int pass, const char *directive, const char *fmt, va_list ap){
	printf("%s %d - ", (pass ? "ok" : "not ok"), ++t_count) ;
	vprintf(fmt, ap) ;
	if (directive != NULL){
		printf(" # %s", directive) ;
	}
	printf("\n") ;
	fflush(stdout) ;
	if (! pass){
		t_failed++ ;
	}
	return pass ;
}


T_FN int ok(int pass, const char *fmt, ...){
	va_list ap ;
	va_start(ap, fmt) ;
	vok(pass, NULL, fmt, ap) ;
	va_end(ap) ;
	return pass ;
}


T_FN void skip(const char *fmt, ...){
	va_list ap ;
	va_start(ap, fmt) ;
	vok(1, "SKIP", fmt, ap) ;
	va_end(ap) ;
}


T_FN int is_int(int64_t got, int64_t want, const char *what){
	if (! ok(got == want, "%s", what)){
		diag("  got %lld, expected %lld", (long long)got, (long long)want) ;
		return 0 ;
	}
	return 1 ;
}


T_FN int is_str(const char *got, const char *want, const char *what){
	int pass = (got != NULL)&&(want != NULL)&&(strcmp(got, want) == 0) ;
	if (! ok(pass, "%s", what)){
		diag("  got '%s', expected '%s'", got ? got : "(null)", want ? want : "(null)") ;
	}
	return pass ;
}


T_FN const char *status_name(ugly_status s){
	static const char *names[] = { "OK", "RAISED", "TYPE", "NOT_FOUND", "NOT_CAPABLE", "STALE_HANDLE",
		"WRONG_THREAD", "IN_USE", "TOO_DEEP", "NOT_SUPPORTED", "NOT_IMPLEMENTED", "USAGE", "NOMEM", "INTERNAL" } ;
	return ((unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "?") ;
}


/* Checks a status; on a mismatch, prints the ctx's error. */
T_FN int is_status(ugly_ctx *ctx, ugly_status got, ugly_status want, const char *what){
	if (! ok(got == want, "%s", what)){
		diag("  got %s, expected %s", status_name(got), status_name(want)) ;
		const ugly_error *e = ugly_last_error(ctx) ;
		if (e != NULL){
			diag("  error: [%s] %s: %s", ugly_error_language(e), ugly_error_type(e), ugly_error_message(e)) ;
		}
		return 0 ;
	}
	return 1 ;
}


T_FN int done_testing(void){
	printf("1..%d\n", t_count) ;
	return (t_failed > 0 ? 1 : 0) ;
}


/* ---------------------------------------------------------------- */
/* The fixture                                                      */
/* ---------------------------------------------------------------- */

typedef struct fixture {
	ugly_ctx *ctx ;
	ugly_lang *lang ;
	ugly_rt *rt ;
	ugly_handle mod ;        /* the "conformance" module */
} fixture ;


T_FN const char *plugin_name(void){
	const char *p = getenv("UGLY_TEST_PLUGIN") ;
	return ((p != NULL)&&(p[0] != '\0')) ? p : "mock" ;
}


/* A second runtime of the same language, with its own module. */
T_FN int fx_runtime(fixture *f, ugly_rt **rt, ugly_handle *mod){
	ugly_status st = ugly_rt_new(f->ctx, f->lang, rt) ;
	if (st == UGLY_OK){
		st = ugly_rt_import(f->ctx, *rt, "conformance", mod) ;
	}
	return st == UGLY_OK ;
}


/*
 * A second runtime of the same language, for tests that need one: a SKIP if
 * the plugin doesn't claim UGLY_CAP_MULTI_RUNTIME, a failure if it claims it
 * but can't.
 */
T_FN int fx_second(fixture *f, ugly_rt **rt, ugly_handle *mod, const char *what){
	if (! (ugly_lang_caps(f->lang) & UGLY_CAP_MULTI_RUNTIME)){
		skip("%s: plugin lacks UGLY_CAP_MULTI_RUNTIME", what) ;
		return 0 ;
	}
	if (! fx_runtime(f, rt, mod)){
		ok(0, "%s: a second runtime", what) ;
		return 0 ;
	}
	return 1 ;
}


/* Sets up ctx, language, runtime and module; bails out of the test (with a plan) if that fails. */
T_FN void fx_setup(fixture *f){
	memset(f, 0, sizeof(*f)) ;
	ugly_status st = ugly_ctx_new(&f->ctx) ;
	if (st == UGLY_OK){
		st = ugly_lang_load(f->ctx, plugin_name(), &f->lang) ;
	}
	if ((st == UGLY_OK)&&(! fx_runtime(f, &f->rt, &f->mod))){
		st = UGLY_ERR_INTERNAL ;
	}
	if (st != UGLY_OK){
		const ugly_error *e = ugly_last_error(f->ctx) ;
		printf("Bail out! can't set up plugin %s: %s\n", plugin_name(), e ? ugly_error_message(e) : status_name(st)) ;
		exit(1) ;
	}
}


T_FN void fx_teardown(fixture *f){
	ugly_release(f->ctx, f->mod) ;
	ugly_status st = ugly_rt_delete(f->ctx, f->rt) ;
	if (st == UGLY_OK){
		st = ugly_lang_unload(f->ctx, f->lang) ;
	}
	if (st != UGLY_OK){
		diag("teardown failed: %s", status_name(st)) ;
		t_failed++ ;
	}
	ugly_ctx_delete(f->ctx) ;
}


/* Calls module function 'name' with args. */
T_FN ugly_status fcall(fixture *f, ugly_handle mod, const char *name, const ugly_val *args, size_t n,
	ugly_kind want, ugly_val *out){

	ugly_val fn ;
	ugly_status st = ugly_get_member(f->ctx, mod, name, UGLY_HANDLE, &fn) ;
	if (st != UGLY_OK){
		*out = ugly_null() ;
		return st ;
	}
	st = ugly_call(f->ctx, fn.as.h, args, n, want, out) ;
	ugly_val_release(f->ctx, &fn) ;
	return st ;
}


/* finalized(): how many fixture objects the runtime has freed. */
T_FN int64_t finalized(fixture *f, ugly_handle mod){
	ugly_val v ;
	if (fcall(f, mod, "finalized", NULL, 0, UGLY_INT, &v) != UGLY_OK){
		return -1 ;
	}
	return v.as.i ;
}


T_FN ugly_handle make_counter(fixture *f, ugly_handle mod){
	ugly_val v ;
	if (fcall(f, mod, "make_counter", NULL, 0, UGLY_HANDLE, &v) != UGLY_OK){
		return UGLY_NO_HANDLE ;
	}
	return v.as.h ;
}


/* A string result as a C string (valid until the value is released). */
T_FN const char *sval(const ugly_val *v){
	return (v->kind == UGLY_STRING ? v->as.str.ptr : "(not a string)") ;
}

#endif
