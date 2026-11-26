/*
 * The Haskell plugin, language "haskell": GHC's runtime system, started in
 * the host's process. The Haskell side (the modules in Ugly/, with rts.c) is
 * libugly_hs.so, built by ghc and linked against GHC's shared libraries and
 * threaded RTS; this file is the vtable, reaching it through its foreign
 * exports.
 *
 * init starts the RTS and checks that the two sides agree; runtimes come and
 * go, from any thread. The reading operations are Haskell's (Ugly.Plugin),
 * on boxes: import (built-in modules only, for now), describe, members and
 * elements. Haskell values are immutable, so writing is refused; calls
 * aren't implemented yet. The RTS is one-way (it can't be started again once
 * stopped), so there's no 'term' and no UGLY_CAP_RESTARTABLE.
 * doc/haskell-bridge.md is the plan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ugly_plugin.h"

#define BRIDGE_ABI 2     /* Ugly.Plugin.bridgeAbi */

/* libugly_hs.so: rts.c, and Ugly.Plugin's foreign exports. */
extern int ugly_hs_start(const char *opts) ;
extern int ugly_hs_abi(void) ;
extern void *ugly_hs_rt_new(void) ;              /* StablePtr Runtime */
extern void ugly_hs_rt_delete(void *rt) ;
/* Ugly.Plugin's entry points, with the vtable's signatures. */
extern ugly_status ugly_hs_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out) ;
extern ugly_status ugly_hs_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name) ;
extern ugly_status ugly_hs_get_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want,
	ugly_val *out) ;
extern ugly_status ugly_hs_member_names(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out) ;
extern ugly_status ugly_hs_length(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out) ;
extern ugly_status ugly_hs_get_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want,
	ugly_val *out) ;
extern void ugly_hs_release(ugly_rt *rt, void *ref) ;


/*
 * The "rts" option: more RTS options (-N4, -M1g), each value one or more
 * words, as between +RTS and -RTS. The RTS also reads GHCRTS.
 */
static char *rts_options(void){
	size_t size = 1 ;
	const char *v ;
	for (size_t i = 0 ; (v = ugly_option("haskell", "rts", i)) != NULL ; i++){
		size += strlen(v) + 1 ;
	}
	char *opts = malloc(size) ;
	if (opts == NULL){
		return NULL ;
	}
	opts[0] = '\0' ;
	for (size_t i = 0 ; (v = ugly_option("haskell", "rts", i)) != NULL ; i++){
		if (i > 0){
			strcat(opts, " ") ;
		}
		strcat(opts, v) ;
	}
	return opts ;
}


static ugly_status hs_init(ugly_ctx *ctx, const char *dir){
	(void)ctx ;
	(void)dir ;
	char *opts = rts_options() ;
	if ((opts == NULL)||(ugly_hs_start(opts) != 0)){
		free(opts) ;
		return UGLY_ERR_NOMEM ;
	}
	free(opts) ;
	if (ugly_hs_abi() != BRIDGE_ABI){
		fprintf(stderr, "ugly haskell: libugly_hs.so doesn't match this plugin (%d)\n", BRIDGE_ABI) ;
		return UGLY_ERR_INTERNAL ;
	}
	return UGLY_OK ;
}


static ugly_status hs_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	(void)ctx ;
	(void)rt ;
	*native = ugly_hs_rt_new() ;
	return UGLY_OK ;
}


static ugly_status hs_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	(void)ctx ;
	(void)rt ;
	ugly_hs_rt_delete(native) ;
	return UGLY_OK ;
}


/* Haskell values are immutable. */
#define IMMUTABLE() return ugly_fail(ctx, rt, UGLY_ERR_NOT_CAPABLE, "TypeError", "Haskell values are immutable")

/* Not yet. */
#define TODO(op) return ugly_fail(ctx, rt, UGLY_ERR_NOT_IMPLEMENTED, "", "haskell: " op " isn't implemented yet")

static ugly_status hs_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_val *out){
	TODO("globals") ;
}
static ugly_status hs_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	IMMUTABLE() ;
}
static ugly_status hs_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	IMMUTABLE() ;
}
static ugly_status hs_call(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out){
	TODO("call") ;
}
static ugly_status hs_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out){
	TODO("invoke") ;
}
static ugly_status hs_new_instance(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_handle *out){
	TODO("new_instance") ;
}
static ugly_status hs_delete_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name){
	IMMUTABLE() ;
}
static ugly_status hs_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n){
	IMMUTABLE() ;
}


static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = UGLY_CAP_MULTI_RUNTIME | UGLY_CAP_ANY_THREAD,
	.name = "haskell",
	.init = hs_init,
	.rt_new = hs_rt_new,
	.rt_delete = hs_rt_delete,
	.import = ugly_hs_import,
	.globals = hs_globals,
	.describe = ugly_hs_describe,
	.get_member = ugly_hs_get_member,
	.set_member = hs_set_member,
	.member_names = ugly_hs_member_names,
	.length = ugly_hs_length,
	.get_index = ugly_hs_get_index,
	.set_index = hs_set_index,
	.call = hs_call,
	.invoke = hs_invoke,
	.new_instance = hs_new_instance,
	.delete_member = hs_delete_member,
	.splice = hs_splice,
	.release = ugly_hs_release,
	.options = (const char *const []){ "rts", NULL },
} ;


const ugly_plugin *ugly_plugin_haskell(void){
	return &plugin ;
}
