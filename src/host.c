/*
 * The built-in "c" language: the home of values the host makes itself.
 * C callables (callbacks), and core lists and maps that any runtime can use
 * through the protocol. It is a plugin like any other, so the core has no
 * special cases for it; each ctx has one runtime of it, on the ctx's thread.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "internal.h"


typedef enum { C_CALLABLE, C_LIST, C_MAP } ckind ;

typedef struct cobj {
	ckind kind ;
	int refs ;                   /* native references: one per handle reference the core holds */

	ugly_c_fn fn ;               /* C_CALLABLE */
	void *data ;
	void (*free_data)(void *) ;

	ugly_val *items ;            /* C_LIST: values; C_MAP: values, parallel to keys */
	char **keys ;
	size_t n, cap ;
} cobj ;


/* ---------------------------------------------------------------- */
/* Stored values: owned copies                                      */
/* ---------------------------------------------------------------- */

static ugly_status store(ugly_ctx *ctx, ugly_val in, ugly_val *slot){
	switch (in.kind){
		case UGLY_STRING:
			return ugly_val_set_string(ctx, slot, in.as.str.ptr, in.as.str.len) ;
		case UGLY_BYTES:
			return ugly_val_set_bytes(ctx, slot, in.as.bytes.ptr, in.as.bytes.len) ;
		case UGLY_HANDLE: {
			ugly_handle h ;
			ugly_status st = h_dup(in.as.h, &h) ;
			if (st == UGLY_OK){
				*slot = ugly_hval(h) ;
			}
			return st ;
		}
		case UGLY_ANY:
			return UGLY_ERR_USAGE ;
		default:
			*slot = in ;
			return UGLY_OK ;
	}
}


static void unstore(ugly_val *slot){
	ugly_val_release(NULL, slot) ;
}


/* The "c" language's conversion rules: exact kinds only, plus lossless INT <-> DOUBLE. */
static ugly_status load(ugly_ctx *ctx, const ugly_val *slot, ugly_kind want, ugly_val *out){
	if ((want == UGLY_ANY)||(want == slot->kind)){
		return store(ctx, *slot, out) ;
	}
	if ((want == UGLY_DOUBLE)&&(slot->kind == UGLY_INT)){
		*out = ugly_double((double)slot->as.i) ;
		return UGLY_OK ;
	}
	if ((want == UGLY_INT)&&(slot->kind == UGLY_DOUBLE)){
		double d = slot->as.d ;
		if ((d == floor(d))&&(d >= -9223372036854775808.0)&&(d < 9223372036854775808.0)){
			*out = ugly_int((int64_t)d) ;
			return UGLY_OK ;
		}
	}
	return ctx_fail(ctx, UGLY_ERR_TYPE, "c", "TypeError", "can't convert stored value to the kind wanted") ;
}


/* ---------------------------------------------------------------- */
/* Objects                                                          */
/* ---------------------------------------------------------------- */

static cobj *cobj_new(ckind kind){
	cobj *o = calloc(1, sizeof(cobj)) ;
	if (o != NULL){
		o->kind = kind ;
		o->refs = 1 ;
	}
	return o ;
}


static int grow(cobj *o){
	if (o->n < o->cap){
		return 1 ;
	}
	size_t ncap = (o->cap ? o->cap * 2 : 8) ;
	ugly_val *ni = realloc(o->items, ncap * sizeof(ugly_val)) ;
	if (ni == NULL){
		return 0 ;
	}
	o->items = ni ;
	if (o->kind == C_MAP){
		char **nk = realloc(o->keys, ncap * sizeof(char *)) ;
		if (nk == NULL){
			return 0 ;
		}
		o->keys = nk ;
	}
	o->cap = ncap ;
	return 1 ;
}


static void cobj_free(cobj *o){
	size_t i ;
	for (i = 0 ; i < o->n ; i++){
		unstore(&o->items[i]) ;
		if (o->kind == C_MAP){
			free(o->keys[i]) ;
		}
	}
	free(o->items) ;
	free(o->keys) ;
	if ((o->kind == C_CALLABLE)&&(o->free_data != NULL)){
		o->free_data(o->data) ;
	}
	free(o) ;
}


static ugly_status cobj_export(ugly_ctx *ctx, cobj *o, ugly_handle *out){
	return ugly_export(ctx, ctx->host, o, (uintptr_t)o, out) ;
}


static long map_find(const cobj *o, const char *name){
	size_t i ;
	for (i = 0 ; i < o->n ; i++){
		if (strcmp(o->keys[i], name) == 0){
			return (long)i ;
		}
	}
	return -1 ;
}


/* ---------------------------------------------------------------- */
/* The plugin                                                       */
/* ---------------------------------------------------------------- */

static ugly_status c_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	(void)ctx ; (void)rt ;
	*native = NULL ;
	return UGLY_OK ;
}


static ugly_status c_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	(void)ctx ; (void)rt ; (void)native ;
	return UGLY_OK ;
}


static ugly_status c_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name){
	(void)rt ;
	cobj *o = ref ;
	const char *name = "function" ;
	switch (o->kind){
		case C_CALLABLE: *traits = UGLY_TRAIT_CALLABLE ; name = "function" ; break ;
		case C_LIST: *traits = UGLY_TRAIT_ELEMENTS ; name = "list" ; break ;
		case C_MAP: *traits = UGLY_TRAIT_MEMBERS ; name = "map" ; break ;
	}
	return ugly_val_set_string(ctx, type_name, name, strlen(name)) ;
}


static ugly_status not_capable(ugly_ctx *ctx, const char *what){
	return ctx_fail(ctx, UGLY_ERR_NOT_CAPABLE, "c", "TypeError", "%s", what) ;
}


static ugly_status c_get_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want, ugly_val *out){
	(void)rt ;
	cobj *o = ref ;
	if (o->kind != C_MAP){
		return not_capable(ctx, "value has no members") ;
	}
	long i = map_find(o, name) ;
	if (i < 0){
		return ctx_fail(ctx, UGLY_ERR_NOT_FOUND, "c", "KeyError", "no member '%s'", name) ;
	}
	return load(ctx, &o->items[i], want, out) ;
}


static ugly_status c_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	(void)rt ;
	cobj *o = ref ;
	if (o->kind != C_MAP){
		return not_capable(ctx, "value has no members") ;
	}
	ugly_val stored ;
	ugly_status st = store(ctx, v, &stored) ;
	if (st != UGLY_OK){
		return ctx_fail(ctx, st, "c", "", "can't store value") ;
	}
	long i = map_find(o, name) ;
	if (i >= 0){
		unstore(&o->items[i]) ;
		o->items[i] = stored ;
		return UGLY_OK ;
	}
	char *key = xstrdup(name) ;
	if ((key == NULL)||(! grow(o))){
		free(key) ;
		unstore(&stored) ;
		return ctx_fail(ctx, UGLY_ERR_NOMEM, "c", "", "out of memory") ;
	}
	o->keys[o->n] = key ;
	o->items[o->n++] = stored ;
	return UGLY_OK ;
}


static ugly_status c_member_names(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	(void)rt ;
	cobj *o = ref ;
	if (o->kind != C_MAP){
		return not_capable(ctx, "value has no members") ;
	}
	ugly_val *names = calloc(o->n + 1, sizeof(ugly_val)) ;
	if (names == NULL){
		return ctx_fail(ctx, UGLY_ERR_NOMEM, "c", "", "out of memory") ;
	}
	size_t i ;
	for (i = 0 ; i < o->n ; i++){
		names[i] = ugly_str(o->keys[i], strlen(o->keys[i])) ;
	}
	ugly_status st = ugly_list_new(ctx, names, o->n, out) ;
	free(names) ;
	return st ;
}


static ugly_status c_length(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out){
	(void)rt ;
	cobj *o = ref ;
	if (o->kind != C_LIST){
		return not_capable(ctx, "value has no elements") ;
	}
	*out = (int64_t)o->n ;
	return UGLY_OK ;
}


static ugly_status c_get_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want, ugly_val *out){
	(void)rt ;
	cobj *o = ref ;
	if (o->kind != C_LIST){
		return not_capable(ctx, "value has no elements") ;
	}
	if ((i < 0)||((uint64_t)i >= o->n)){
		return ctx_fail(ctx, UGLY_ERR_NOT_FOUND, "c", "IndexError", "index %lld out of range", (long long)i) ;
	}
	return load(ctx, &o->items[i], want, out) ;
}


static ugly_status c_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	(void)rt ;
	cobj *o = ref ;
	if (o->kind != C_LIST){
		return not_capable(ctx, "value has no elements") ;
	}
	if ((i < 0)||((uint64_t)i > o->n)){
		return ctx_fail(ctx, UGLY_ERR_NOT_FOUND, "c", "IndexError", "index %lld out of range", (long long)i) ;
	}
	ugly_val stored ;
	ugly_status st = store(ctx, v, &stored) ;
	if (st != UGLY_OK){
		return ctx_fail(ctx, st, "c", "", "can't store value") ;
	}
	if ((uint64_t)i == o->n){     /* one past the end appends */
		if (! grow(o)){
			unstore(&stored) ;
			return ctx_fail(ctx, UGLY_ERR_NOMEM, "c", "", "out of memory") ;
		}
		o->items[o->n++] = stored ;
		return UGLY_OK ;
	}
	unstore(&o->items[i]) ;
	o->items[i] = stored ;
	return UGLY_OK ;
}


static ugly_status c_call(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out){

	(void)rt ;
	cobj *o = ref ;
	if (o->kind != C_CALLABLE){
		return not_capable(ctx, "value is not callable") ;
	}
	return o->fn(ctx, o->data, args, nargs, want, out) ;
}


/* Only maps have methods: a member holding a callable. */
static ugly_status c_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out){

	ugly_val m ;
	ugly_status st = c_get_member(ctx, rt, ref, method, UGLY_HANDLE, &m) ;
	if (st != UGLY_OK){
		return st ;
	}
	st = ugly_call(ctx, m.as.h, args, nargs, want, out) ;
	ugly_val_release(ctx, &m) ;
	return st ;
}


static void c_release(ugly_rt *rt, void *ref){
	(void)rt ;
	cobj *o = ref ;
	if (--o->refs == 0){
		cobj_free(o) ;
	}
}


const ugly_plugin ugly_c_plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = UGLY_CAP_MULTI_RUNTIME,
	.name = "c",
	.rt_new = c_rt_new,
	.rt_delete = c_rt_delete,
	.describe = c_describe,
	.get_member = c_get_member,
	.set_member = c_set_member,
	.member_names = c_member_names,
	.length = c_length,
	.get_index = c_get_index,
	.set_index = c_set_index,
	.call = c_call,
	.invoke = c_invoke,
	.release = c_release,
} ;


/* ---------------------------------------------------------------- */
/* Public constructors                                              */
/* ---------------------------------------------------------------- */

ugly_status ugly_callable_new(ugly_ctx *ctx, ugly_c_fn fn, void *data, void (*free_data)(void *), ugly_handle *out){
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if ((fn == NULL)||(out == NULL)){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_callable_new: fn and out are required") ;
	}
	cobj *o = cobj_new(C_CALLABLE) ;
	if (o == NULL){
		return ctx_fail(ctx, UGLY_ERR_NOMEM, "core", "", "out of memory") ;
	}
	o->fn = fn ;
	o->data = data ;
	o->free_data = free_data ;
	return cobj_export(ctx, o, out) ;
}


ugly_status ugly_list_new(ugly_ctx *ctx, const ugly_val *items, size_t n, ugly_handle *out){
	/* Also used by plugins mid-call (member_names): don't clear a ctx error that isn't ours. */
	if ((ctx == NULL)||(! on_thread(ctx->thread))){
		return (ctx == NULL ? UGLY_ERR_USAGE : UGLY_ERR_WRONG_THREAD) ;
	}
	if ((out == NULL)||((n > 0)&&(items == NULL))){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_list_new: invalid arguments") ;
	}
	cobj *o = cobj_new(C_LIST) ;
	if (o == NULL){
		return ctx_fail(ctx, UGLY_ERR_NOMEM, "core", "", "out of memory") ;
	}
	size_t i ;
	for (i = 0 ; i < n ; i++){
		ugly_status st = UGLY_ERR_NOMEM ;
		if ((! grow(o))||((st = store(ctx, items[i], &o->items[o->n])) != UGLY_OK)){
			cobj_free(o) ;
			return ctx_fail(ctx, st, "core", "", "can't store list item %zu", i) ;
		}
		o->n++ ;
	}
	return cobj_export(ctx, o, out) ;
}


ugly_status ugly_map_new(ugly_ctx *ctx, ugly_handle *out){
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if (out == NULL){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_map_new: out is required") ;
	}
	cobj *o = cobj_new(C_MAP) ;
	if (o == NULL){
		return ctx_fail(ctx, UGLY_ERR_NOMEM, "core", "", "out of memory") ;
	}
	return cobj_export(ctx, o, out) ;
}
