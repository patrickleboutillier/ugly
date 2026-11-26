/*
 * TAP helpers for the Haskell plugin's tests (values.c, calls.c,
 * exports.c), until it joins the conformance suite.
 */
#ifndef HASKELL_TAP_H
#define HASKELL_TAP_H

#define _GNU_SOURCE     /* RTLD_DEFAULT */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ugly.h"

static int n, failed ;
static ugly_ctx *ctx ;

static inline void ok(int cond, const char *what){
	printf("%sok %d - %s\n", cond ? "" : "not ", ++n, what) ;
	failed |= ! cond ;
}

static inline int is_string(ugly_val v, const char *s){
	return (v.kind == UGLY_STRING)&&(v.as.str.len == strlen(s))&&(memcmp(v.as.str.ptr, s, v.as.str.len) == 0) ;
}

static inline int is_int(ugly_val v, int64_t i){
	return (v.kind == UGLY_INT)&&(v.as.i == i) ;
}

/* describe: these traits and this type name. */
static inline int type_is(ugly_handle h, uint32_t traits, const char *name){
	uint32_t t = 99 ;
	ugly_val tn = ugly_null() ;
	int good = (ugly_describe(ctx, h, &t, &tn) == UGLY_OK)&&(t == traits)&&is_string(tn, name) ;
	if (! good){
		printf("# describe: traits %u, type %.*s\n", t, tn.kind == UGLY_STRING ? (int)tn.as.str.len : 0,
			tn.kind == UGLY_STRING ? tn.as.str.ptr : "") ;
	}
	ugly_val_release(ctx, &tn) ;
	return good ;
}

/* The status, and the current error: Haskell's, with this type and a message containing this. */
static inline int error_is(ugly_status st, ugly_status expect, const char *type, const char *message){
	const ugly_error *e = ugly_last_error(ctx) ;
	int good = (st == expect)&&(e != NULL)&&(strcmp(ugly_error_language(e), "haskell") == 0)
		&&(strcmp(ugly_error_type(e), type) == 0)&&(strstr(ugly_error_message(e), message) != NULL) ;
	if (! good){
		printf("# status %d, error %s %s: %s\n", st, e ? ugly_error_language(e) : "-", e ? ugly_error_type(e) : "-",
			e ? ugly_error_message(e) : "-") ;
	}
	return good ;
}

/* After the runtime is deleted: no ref left on the Haskell side (valgrind can't see them). */
static inline void all_released(void){
	int (*live_refs)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "ugly_hs_live_refs") ;
	ok((live_refs != NULL)&&(live_refs() == 0), "every ref released") ;
	if (live_refs != NULL){
		printf("# %d refs live\n", live_refs()) ;
	}
}

/* Loads haskell, makes a runtime and imports 'module'; ends the test if it can't. */
static inline ugly_rt *start(const char *module, ugly_handle *mod){
	ugly_lang *lang = NULL ;
	ugly_rt *rt = NULL ;
	setvbuf(stdout, NULL, _IOLBF, 0) ;
	ugly_ctx_new(&ctx) ;
	if ((ugly_lang_load(ctx, "haskell", &lang) != UGLY_OK)||(ugly_rt_new(ctx, lang, &rt) != UGLY_OK)
		||(ugly_rt_import(ctx, rt, module, mod) != UGLY_OK)){
		printf("not ok 1 - haskell starts, and imports %s\n1..1\n", module) ;
		exit(1) ;
	}
	return rt ;
}

#endif
