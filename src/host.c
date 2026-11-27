/*
 * The built-in "c" language: the home of values the host makes itself.
 * C callables (callbacks), and core lists and maps that any runtime can use
 * through the protocol (lists are iterable too, by an iterator object). It is a plugin like any other, so the core has no
 * special cases for it; each ctx has one runtime of it, on the ctx's thread.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "internal.h"


typedef enum { C_CALLABLE, C_LIST, C_MAP, C_ITER } ckind ;

typedef struct cobj {
	ckind kind ;
	int refs ;                   /* native references: one per handle reference the core holds */

	ugly_c_fn fn ;               /* C_CALLABLE */
	void *data ;
	void (*free_data)(void *) ;

	ugly_val *items ;            /* C_LIST: values; C_MAP: values, parallel to keys */
	char **keys ;
	size_t n, cap ;

	struct cobj *list ;          /* C_ITER: the list (one native reference), and the next index */
	size_t pos ;
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


static void c_release(ugly_rt *rt, void *ref) ;

static void cobj_free(cobj *o){
	size_t i ;
	if (o->kind == C_ITER){
		c_release(NULL, o->list) ;
	}
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
		case C_LIST: *traits = UGLY_TRAIT_ELEMENTS | UGLY_TRAIT_ITERABLE ; name = "list" ; break ;
		case C_MAP: *traits = UGLY_TRAIT_MEMBERS | UGLY_TRAIT_ENTRIES ; name = "map" ; break ;
		case C_ITER: *traits = UGLY_TRAIT_ITERABLE ; name = "iterator" ; break ;
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


static ugly_status c_delete_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name){
	(void)rt ;
	cobj *o = ref ;
	if (o->kind != C_MAP){
		return not_capable(ctx, "value has no members") ;
	}
	long i = map_find(o, name) ;
	if (i >= 0){
		ugly_val gone = o->items[i] ;
		free(o->keys[i]) ;
		memmove(&o->keys[i], &o->keys[i + 1], (o->n - (size_t)i - 1) * sizeof(char *)) ;
		memmove(&o->items[i], &o->items[i + 1], (o->n - (size_t)i - 1) * sizeof(ugly_val)) ;
		o->n-- ;
		unstore(&gone) ;
	}
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


static ugly_status c_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n){

	(void)rt ;
	cobj *o = ref ;
	if (o->kind != C_LIST){
		return not_capable(ctx, "value has no elements") ;
	}
	size_t off = (size_t)offset, rm = (size_t)remove, nn = o->n - rm + n, i ;
	ugly_val *stored = calloc(n + 1, sizeof(ugly_val)) ;
	ugly_val *gone = calloc(rm + 1, sizeof(ugly_val)) ;
	if ((stored == NULL)||(gone == NULL)){
		free(stored) ;
		free(gone) ;
		return ctx_fail(ctx, UGLY_ERR_NOMEM, "c", "", "out of memory") ;
	}
	for (i = 0 ; i < n ; i++){
		ugly_status st = store(ctx, insert[i], &stored[i]) ;
		if (st != UGLY_OK){
			while (i-- > 0){
				unstore(&stored[i]) ;
			}
			free(stored) ;
			free(gone) ;
			return ctx_fail(ctx, st, "c", "", "can't store value") ;
		}
	}
	if (nn > o->cap){
		ugly_val *ni = realloc(o->items, nn * sizeof(ugly_val)) ;
		if (ni == NULL){
			for (i = 0 ; i < n ; i++){
				unstore(&stored[i]) ;
			}
			free(stored) ;
			free(gone) ;
			return ctx_fail(ctx, UGLY_ERR_NOMEM, "c", "", "out of memory") ;
		}
		o->items = ni ;
		o->cap = nn ;
	}
	memcpy(gone, &o->items[off], rm * sizeof(ugly_val)) ;
	memmove(&o->items[off + n], &o->items[off + rm], (o->n - off - rm) * sizeof(ugly_val)) ;
	memcpy(&o->items[off], stored, n * sizeof(ugly_val)) ;
	o->n = nn ;
	for (i = 0 ; i < rm ; i++){     /* last: releasing may run finalizers */
		unstore(&gone[i]) ;
	}
	free(stored) ;
	free(gone) ;
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


/*
 * A core map's entries are its members: it is a record and a dictionary at
 * once, as a Lua table is, with string keys. Another kind of key isn't
 * there, and can't be set.
 */
static const char *map_key(const cobj *o, ugly_val key){
	return ((o->kind == C_MAP)&&(key.kind == UGLY_STRING)&&(memchr(key.as.str.ptr, '\0', key.as.str.len) == NULL)
		? key.as.str.ptr : NULL) ;
}

static ugly_status c_get_entry(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_val key, ugly_kind want, ugly_val *out){
	cobj *o = ref ;
	if (o->kind != C_MAP){
		return not_capable(ctx, "value has no entries") ;
	}
	char *k = (map_key(o, key) != NULL ? strndup(key.as.str.ptr, key.as.str.len) : NULL) ;
	if (k == NULL){
		return ctx_fail(ctx, UGLY_ERR_NOT_FOUND, "c", "KeyError", "no such entry") ;
	}
	ugly_status st = c_get_member(ctx, rt, ref, k, want, out) ;
	free(k) ;
	return st ;
}

static ugly_status c_set_entry(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_val key, ugly_val v){
	cobj *o = ref ;
	if (o->kind != C_MAP){
		return not_capable(ctx, "value has no entries") ;
	}
	char *k = (map_key(o, key) != NULL ? strndup(key.as.str.ptr, key.as.str.len) : NULL) ;
	if (k == NULL){
		return ctx_fail(ctx, UGLY_ERR_TYPE, "c", "TypeError", "a core map's keys are strings") ;
	}
	ugly_status st = c_set_member(ctx, rt, ref, k, v) ;
	free(k) ;
	return st ;
}

static ugly_status c_delete_entry(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_val key){
	cobj *o = ref ;
	if (o->kind != C_MAP){
		return not_capable(ctx, "value has no entries") ;
	}
	char *k = (map_key(o, key) != NULL ? strndup(key.as.str.ptr, key.as.str.len) : NULL) ;
	if (k == NULL){
		return UGLY_OK ;
	}
	ugly_status st = c_delete_member(ctx, rt, ref, k) ;
	free(k) ;
	return st ;
}

static ugly_status c_entry_count(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out){
	(void)rt ;
	cobj *o = ref ;
	if (o->kind != C_MAP){
		return not_capable(ctx, "value has no entries") ;
	}
	*out = (int64_t)o->n ;
	return UGLY_OK ;
}

/* The keys as they are now: an iterator over a list of them. */
static ugly_status c_entry_keys(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	ugly_handle names = UGLY_NO_HANDLE ;
	ugly_status st = c_member_names(ctx, rt, ref, &names) ;
	if (st != UGLY_OK){
		return st ;
	}
	st = ugly_iter(ctx, names, out) ;
	ugly_error *kept = (st != UGLY_OK ? ugly_take_error(ctx) : NULL) ;
	ugly_release(ctx, names) ;
	if (kept != NULL){
		ugly_restore_error(ctx, kept) ;
	}
	return st ;
}


/* A list gives an iterator over its indexes; an iterator gives itself. */
static ugly_status c_iter(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	cobj *o = ref ;
	if (o->kind == C_ITER){
		o->refs++ ;      /* the export's reference; given back, since the handle is live */
		return ugly_export(ctx, rt, o, (uintptr_t)o, out) ;
	}
	if (o->kind != C_LIST){
		return not_capable(ctx, "value is not iterable") ;
	}
	cobj *it = cobj_new(C_ITER) ;
	if (it == NULL){
		return ctx_fail(ctx, UGLY_ERR_NOMEM, "core", "", "out of memory") ;
	}
	o->refs++ ;
	it->list = o ;
	return ugly_export(ctx, rt, it, (uintptr_t)it, out) ;
}


/* Reads the list as it is now, like Python's list iterator: splicing it meanwhile moves what comes next. */
static ugly_status c_next(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_kind want, ugly_val *out, size_t max,
	size_t *got){

	(void)rt ;
	cobj *o = ref ;
	*got = 0 ;
	if (o->kind != C_ITER){
		return not_capable(ctx, "value is not an iterator") ;
	}
	while ((*got < max)&&(o->pos < o->list->n)){
		ugly_status st = load(ctx, &o->list->items[o->pos], want, &out[*got]) ;
		if (st != UGLY_OK){
			return st ;
		}
		o->pos++ ;
		(*got)++ ;
	}
	return UGLY_OK ;
}


/* C has no stringification of its own. */
static ugly_status c_to_string(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_val *out){
	(void)rt ; (void)ref ; (void)out ;
	return ctx_fail(ctx, UGLY_ERR_NOT_SUPPORTED, "c", "TypeError", "C values have no string form") ;
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
	.delete_member = c_delete_member,
	.splice = c_splice,
	.call = c_call,
	.invoke = c_invoke,
	.release = c_release,
	.iter = c_iter,
	.next = c_next,
	.to_string = c_to_string,
	.get_entry = c_get_entry,
	.set_entry = c_set_entry,
	.delete_entry = c_delete_entry,
	.entry_count = c_entry_count,
	.entry_keys = c_entry_keys,
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
