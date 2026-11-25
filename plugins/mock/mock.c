/*
 * The mock plugin: a tiny "language" written in C, with its own object model
 * (modules, functions, counters, lists, a Point class, exceptions), its own
 * conversion rules (lenient, Perl-like: "42" numifies), and proxies for
 * foreign values. It exists to test the core and the contract before any
 * real interpreter is involved, and it follows every plugin rule in
 * ugly_plugin.h.
 *
 * Its "conformance" module provides the fixtures of contract.md section 9.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "ugly_plugin.h"


/* ---------------------------------------------------------------- */
/* The mock language's native values                                */
/* ---------------------------------------------------------------- */

typedef enum { M_MODULE, M_FUNC, M_COUNTER, M_LIST, M_CLASS, M_POINT, M_EXC, M_PROXY, M_KWBOX } mkind ;

typedef struct mobj mobj ;
typedef struct mock_rt mock_rt ;

/* A native value: a primitive or a reference to an object. */
typedef struct mval {
	ugly_kind kind ;               /* never UGLY_ANY; UGLY_HANDLE means 'o' */
	union {
		bool b ;
		int64_t i ;
		double d ;
		struct { char *p ; size_t len ; } s ;    /* STRING and BYTES, owned */
		mobj *o ;                                 /* holds one reference */
	} as ;
} mval ;

typedef ugly_status (*mock_fn)(ugly_ctx *ctx, mock_rt *m, mval *args, size_t n, mval *ret) ;

struct mobj {
	mkind kind ;
	int refs ;
	mock_rt *m ;

	const char *name ;   /* M_FUNC */
	mock_fn fn ;

	int64_t count ;      /* M_COUNTER */
	mobj *items ;

	mval *vals ;         /* M_LIST */
	size_t n, cap ;

	double x, y ;        /* M_POINT */

	char *type ;         /* M_EXC */
	char *message ;

	ugly_handle h ;      /* M_PROXY: one owned reference to a foreign value */
} ;

struct mock_rt {
	ugly_rt *urt ;
	mobj *module ;       /* the "conformance" module, cached: same handle on every import */
	int64_t finalized ;  /* objects freed so far */
	int64_t live ;       /* objects alive now */
} ;


static mval mnull(void){ mval v ; memset(&v, 0, sizeof(v)) ; v.kind = UGLY_NULL ; return v ; }
static mval mint(int64_t i){ mval v = mnull() ; v.kind = UGLY_INT ; v.as.i = i ; return v ; }
static mval mdouble(double d){ mval v = mnull() ; v.kind = UGLY_DOUBLE ; v.as.d = d ; return v ; }
static mval mobjval(mobj *o){ mval v = mnull() ; v.kind = UGLY_HANDLE ; v.as.o = o ; return v ; }

static mval mstr(ugly_kind kind, const char *p, size_t len){
	mval v = mnull() ;
	v.kind = kind ;
	v.as.s.p = malloc(len + 1) ;
	if (len > 0){
		memcpy(v.as.s.p, p, len) ;
	}
	v.as.s.p[len] = '\0' ;
	v.as.s.len = len ;
	return v ;
}


static void obj_unref(mobj *o) ;

static void mval_free(mval *v){
	if ((v->kind == UGLY_STRING)||(v->kind == UGLY_BYTES)){
		free(v->as.s.p) ;
	}
	else if (v->kind == UGLY_HANDLE){
		obj_unref(v->as.o) ;
	}
	*v = mnull() ;
}


static mval mval_copy(const mval *v){
	if ((v->kind == UGLY_STRING)||(v->kind == UGLY_BYTES)){
		return mstr(v->kind, v->as.s.p, v->as.s.len) ;
	}
	if (v->kind == UGLY_HANDLE){
		v->as.o->refs++ ;
	}
	return *v ;
}


static mobj *obj_new(mock_rt *m, mkind kind){
	mobj *o = calloc(1, sizeof(mobj)) ;
	o->kind = kind ;
	o->refs = 1 ;
	o->m = m ;
	m->live++ ;
	return o ;
}


static void obj_unref(mobj *o){
	if (--o->refs > 0){
		return ;
	}
	size_t i ;
	switch (o->kind){
		case M_COUNTER:
			obj_unref(o->items) ;
			break ;
		case M_MODULE:
		case M_LIST:
			for (i = 0 ; i < o->n ; i++){
				mval_free(&o->vals[i]) ;
			}
			free(o->vals) ;
			break ;
		case M_EXC:
			free(o->type) ;
			free(o->message) ;
			break ;
		case M_PROXY:
			ugly_release(ugly_current_ctx(), o->h) ;
			break ;
		default:
			break ;
	}
	o->m->finalized++ ;
	o->m->live-- ;
	free(o) ;
}


static void list_push(mobj *l, mval v){
	if (l->n == l->cap){
		l->cap = (l->cap ? l->cap * 2 : 8) ;
		l->vals = realloc(l->vals, l->cap * sizeof(mval)) ;
	}
	l->vals[l->n++] = v ;
}


/* A language-level exception without an exception object. */
static ugly_status raise(ugly_ctx *ctx, mock_rt *m, const char *type, const char *message){
	return ugly_raise_native(ctx, m->urt, type, message, NULL, 0) ;
}


/* A failure with a specific status (no such member, wrong type...). */
static ugly_status fail(ugly_ctx *ctx, mock_rt *m, ugly_status status, const char *type, const char *message){
	return ugly_fail(ctx, m->urt, status, type, message) ;
}


/* ---------------------------------------------------------------- */
/* Crossing the boundary (rules 2-6)                                */
/* ---------------------------------------------------------------- */

/* An argument (borrowed) to a native value. Our own handles resolve to the original object (rule 5). */
static ugly_status to_native(ugly_ctx *ctx, mock_rt *m, ugly_val in, mval *out){
	*out = mnull() ;
	switch (in.kind){
		case UGLY_NULL: return UGLY_OK ;
		case UGLY_BOOL: out->kind = UGLY_BOOL ; out->as.b = in.as.b ; return UGLY_OK ;
		case UGLY_INT: *out = mint(in.as.i) ; return UGLY_OK ;
		case UGLY_DOUBLE: *out = mdouble(in.as.d) ; return UGLY_OK ;
		case UGLY_STRING: *out = mstr(UGLY_STRING, in.as.str.ptr, in.as.str.len) ; return UGLY_OK ;
		case UGLY_BYTES: *out = mstr(UGLY_BYTES, (const char *)in.as.bytes.ptr, in.as.bytes.len) ; return UGLY_OK ;
		case UGLY_HANDLE: {
			void *ref = NULL ;
			ugly_status st = ugly_resolve(ctx, m->urt, in.as.h, &ref) ;
			if (st != UGLY_OK){
				return st ;
			}
			if (ref != NULL){
				mobj *o = ref ;
				o->refs++ ;
				*out = mobjval(o) ;
				return UGLY_OK ;
			}
			/* Foreign: a proxy that keeps its own reference (rule 2: dup before keeping). */
			ugly_handle h ;
			st = ugly_dup(ctx, in.as.h, &h) ;
			if (st != UGLY_OK){
				return st ;
			}
			mobj *p = obj_new(m, M_PROXY) ;
			p->h = h ;
			*out = mobjval(p) ;
			return UGLY_OK ;
		}
		default:
			return UGLY_ERR_USAGE ;
	}
}


static int parse_int(const char *s, int64_t *out){
	char *end ;
	errno = 0 ;
	long long v = strtoll(s, &end, 10) ;
	if ((*s == '\0')||(*end != '\0')||(errno != 0)){
		return 0 ;
	}
	*out = v ;
	return 1 ;
}


static int parse_double(const char *s, double *out){
	char *end ;
	errno = 0 ;
	double d = strtod(s, &end) ;
	if ((*s == '\0')||(*end != '\0')||(errno != 0)){
		return 0 ;
	}
	*out = d ;
	return 1 ;
}


/* The mock language's conversion rules for 'want' (lenient: strings numify, numbers stringify). */
static ugly_status coerce(ugly_ctx *ctx, mock_rt *m, const mval *v, ugly_kind want, mval *out){
	if ((want == UGLY_ANY)||(want == v->kind)){
		*out = mval_copy(v) ;
		return UGLY_OK ;
	}
	char buf[64] ;
	switch (want){
		case UGLY_INT:
			if (v->kind == UGLY_BOOL){ *out = mint(v->as.b ? 1 : 0) ; return UGLY_OK ; }
			if ((v->kind == UGLY_DOUBLE)&&(v->as.d == floor(v->as.d))&&(fabs(v->as.d) < 9.2e18)){
				*out = mint((int64_t)v->as.d) ;
				return UGLY_OK ;
			}
			if ((v->kind == UGLY_STRING)&&(parse_int(v->as.s.p, &out->as.i))){
				*out = mint(out->as.i) ;
				return UGLY_OK ;
			}
			break ;
		case UGLY_DOUBLE:
			if (v->kind == UGLY_INT){ *out = mdouble((double)v->as.i) ; return UGLY_OK ; }
			if ((v->kind == UGLY_STRING)&&(parse_double(v->as.s.p, &out->as.d))){
				*out = mdouble(out->as.d) ;
				return UGLY_OK ;
			}
			break ;
		case UGLY_BOOL:
			if (v->kind == UGLY_INT){ *out = mnull() ; out->kind = UGLY_BOOL ; out->as.b = (v->as.i != 0) ; return UGLY_OK ; }
			break ;
		case UGLY_STRING:
			if (v->kind == UGLY_INT){
				snprintf(buf, sizeof(buf), "%lld", (long long)v->as.i) ;
				*out = mstr(UGLY_STRING, buf, strlen(buf)) ;
				return UGLY_OK ;
			}
			if (v->kind == UGLY_DOUBLE){
				snprintf(buf, sizeof(buf), "%.17g", v->as.d) ;
				*out = mstr(UGLY_STRING, buf, strlen(buf)) ;
				return UGLY_OK ;
			}
			if (v->kind == UGLY_BYTES){
				*out = mstr(UGLY_STRING, v->as.s.p, v->as.s.len) ;
				return UGLY_OK ;
			}
			break ;
		case UGLY_BYTES:
			if (v->kind == UGLY_STRING){
				*out = mstr(UGLY_BYTES, v->as.s.p, v->as.s.len) ;
				return UGLY_OK ;
			}
			break ;
		default:
			break ;
	}
	return fail(ctx, m, UGLY_ERR_TYPE, "TypeError", "value can't be converted to the kind wanted") ;
}


/* A native value to a result (owned by the caller). Proxies unwrap to their handle (rule 5); objects export with identity (rule 6). */
static ugly_status from_native(ugly_ctx *ctx, mock_rt *m, const mval *v, ugly_kind want, ugly_val *out){
	mval c ;
	ugly_status st = coerce(ctx, m, v, want, &c) ;
	if (st != UGLY_OK){
		return st ;
	}
	switch (c.kind){
		case UGLY_NULL: *out = ugly_null() ; break ;
		case UGLY_BOOL: *out = ugly_bool(c.as.b) ; break ;
		case UGLY_INT: *out = ugly_int(c.as.i) ; break ;
		case UGLY_DOUBLE: *out = ugly_double(c.as.d) ; break ;
		case UGLY_STRING: st = ugly_val_set_string(ctx, out, c.as.s.p, c.as.s.len) ; break ;
		case UGLY_BYTES: st = ugly_val_set_bytes(ctx, out, (const uint8_t *)c.as.s.p, c.as.s.len) ; break ;
		case UGLY_HANDLE: {
			mobj *o = c.as.o ;
			ugly_handle h ;
			if (o->kind == M_PROXY){
				st = ugly_dup(ctx, o->h, &h) ;
			}
			else {
				o->refs++ ;    /* the handle's reference; ugly_export gives it back if it's a duplicate */
				st = ugly_export(ctx, m->urt, o, (uintptr_t)o, &h) ;
			}
			if (st == UGLY_OK){
				*out = ugly_hval(h) ;
			}
			break ;
		}
		default:
			st = UGLY_ERR_INTERNAL ;
	}
	mval_free(&c) ;
	return st ;
}


/* Calls a callable native value: a mock function directly, a foreign one through the core. */
static ugly_status mock_call(ugly_ctx *ctx, mock_rt *m, const mval *f, mval *args, size_t n, mval *ret){
	*ret = mnull() ;
	if ((f->kind == UGLY_HANDLE)&&(f->as.o->kind == M_FUNC)){
		return f->as.o->fn(ctx, m, args, n, ret) ;
	}
	if ((f->kind != UGLY_HANDLE)||(f->as.o->kind != M_PROXY)){
		return fail(ctx, m, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not callable") ;
	}

	ugly_val *uargs = calloc(n + 1, sizeof(ugly_val)) ;
	ugly_status st = UGLY_OK ;
	size_t i, made = 0 ;
	for (i = 0 ; (i < n)&&(st == UGLY_OK) ; i++, made++){
		st = from_native(ctx, m, &args[i], UGLY_ANY, &uargs[i]) ;
	}
	ugly_val r = ugly_null() ;
	if (st == UGLY_OK){
		st = ugly_call(ctx, f->as.o->h, uargs, n, UGLY_ANY, &r) ;
	}
	for (i = 0 ; i < made ; i++){
		ugly_val_release(ctx, &uargs[i]) ;
	}
	free(uargs) ;
	if (st != UGLY_OK){
		return st ;     /* the error stays in ctx, unchanged (rule 7) */
	}
	st = to_native(ctx, m, r, ret) ;
	ugly_val_release(ctx, &r) ;
	return st ;
}


/* ---------------------------------------------------------------- */
/* The "conformance" module: fixtures                               */
/* ---------------------------------------------------------------- */

#define ARGS(k) do { if (n != (k)){ return raise(ctx, m, "ArgumentError", "wrong number of arguments") ; } } while (0)

static ugly_status f_echo(ugly_ctx *ctx, mock_rt *m, mval *a, size_t n, mval *ret){
	ARGS(1) ;
	*ret = mval_copy(&a[0]) ;
	return UGLY_OK ;
}


static const char *kind_of(const mval *v){
	static const char *objs[] = { "module", "function", "counter", "list", "class", "point", "exception", "proxy", "kwbox" } ;
	switch (v->kind){
		case UGLY_NULL: return "null" ;
		case UGLY_BOOL: return "bool" ;
		case UGLY_INT: return "int" ;
		case UGLY_DOUBLE: return "double" ;
		case UGLY_STRING: return "string" ;
		case UGLY_BYTES: return "bytes" ;
		case UGLY_HANDLE: return objs[v->as.o->kind] ;
		default: return "?" ;
	}
}


static ugly_status f_kind_of(ugly_ctx *ctx, mock_rt *m, mval *a, size_t n, mval *ret){
	ARGS(1) ;
	const char *k = kind_of(&a[0]) ;
	*ret = mstr(UGLY_STRING, k, strlen(k)) ;
	return UGLY_OK ;
}


static ugly_status f_make_counter(ugly_ctx *ctx, mock_rt *m, mval *a, size_t n, mval *ret){
	(void)a ;
	ARGS(0) ;
	mobj *c = obj_new(m, M_COUNTER) ;
	c->items = obj_new(m, M_LIST) ;
	*ret = mobjval(c) ;
	return UGLY_OK ;
}


static ugly_status f_apply(ugly_ctx *ctx, mock_rt *m, mval *a, size_t n, mval *ret){
	ARGS(2) ;
	return mock_call(ctx, m, &a[0], &a[1], 1, ret) ;
}


static ugly_status f_call_twice(ugly_ctx *ctx, mock_rt *m, mval *a, size_t n, mval *ret){
	ARGS(2) ;
	mval once ;
	ugly_status st = mock_call(ctx, m, &a[0], &a[1], 1, &once) ;
	if (st != UGLY_OK){
		return st ;
	}
	st = mock_call(ctx, m, &a[0], &once, 1, ret) ;
	mval_free(&once) ;
	return st ;
}


static ugly_status f_raise_error(ugly_ctx *ctx, mock_rt *m, mval *a, size_t n, mval *ret){
	(void)ret ;
	ARGS(1) ;
	if (a[0].kind != UGLY_STRING){
		return raise(ctx, m, "TypeError", "raise_error wants a string") ;
	}
	mobj *e = obj_new(m, M_EXC) ;
	e->type = strdup("ConformanceError") ;
	e->message = strdup(a[0].as.s.p) ;
	/* The exception object is the payload: its reference goes to the error. */
	return ugly_raise_native(ctx, m->urt, e->type, e->message, e, (uintptr_t)e) ;
}


static ugly_status f_catch_and_return(ugly_ctx *ctx, mock_rt *m, mval *a, size_t n, mval *ret){
	ARGS(1) ;
	ugly_status st = mock_call(ctx, m, &a[0], NULL, 0, ret) ;
	if (st != UGLY_ERR_RAISED){
		return st ;
	}
	/* Caught: the result is the exception object itself, the original if it is ours. */
	ugly_error *err = ugly_take_error(ctx) ;
	ugly_handle payload = UGLY_NO_HANDLE ;
	st = ugly_error_payload(ctx, err, &payload) ;
	if ((st == UGLY_OK)&&(payload != UGLY_NO_HANDLE)){
		st = to_native(ctx, m, ugly_hval(payload), ret) ;
		ugly_release(ctx, payload) ;
	}
	ugly_error_free(err) ;
	return st ;
}


static ugly_status f_rethrow(ugly_ctx *ctx, mock_rt *m, mval *a, size_t n, mval *ret){
	(void)ret ;
	ARGS(1) ;
	if ((a[0].kind != UGLY_HANDLE)||((a[0].as.o->kind != M_EXC)&&(a[0].as.o->kind != M_PROXY))){
		return raise(ctx, m, "TypeError", "rethrow wants an exception") ;
	}
	mobj *e = a[0].as.o ;
	if (e->kind == M_EXC){
		/* Ours: throw the original object again (rule 7). */
		e->refs++ ;
		return ugly_raise_native(ctx, m->urt, e->type, e->message, e, (uintptr_t)e) ;
	}
	/* Foreign: the payload is the proxied value itself, not a proxy of it. */
	ugly_handle h ;
	ugly_status st = ugly_dup(ctx, e->h, &h) ;
	if (st != UGLY_OK){
		return st ;
	}
	return ugly_raise_handle(ctx, m->urt, "ForeignError", "rethrown foreign exception", h) ;
}


static ugly_status f_finalized(ugly_ctx *ctx, mock_rt *m, mval *a, size_t n, mval *ret){
	(void)a ;
	ARGS(0) ;
	*ret = mint(m->finalized) ;
	return UGLY_OK ;
}


static ugly_status f_inits(ugly_ctx *ctx, mock_rt *m, mval *a, size_t n, mval *ret){
	(void)a ;
	ARGS(0) ;
	const char *s = getenv("UGLY_MOCK_INITS") ;
	*ret = mint(s != NULL ? atoll(s) : 0) ;
	return UGLY_OK ;
}


/*
 * Keyword arguments reach the mock as trailing name, value pairs (its call
 * options, like Perl's). The value for 'key' among a[from..n), or dflt.
 */
static ugly_status pair_int(ugly_ctx *ctx, mock_rt *m, mval *a, size_t from, size_t n, const char *key,
	int64_t dflt, int64_t *out){

	*out = dflt ;
	if ((n - from) % 2 != 0){
		return raise(ctx, m, "ArgumentError", "odd number of name, value arguments") ;
	}
	size_t i ;
	for (i = from ; i + 1 < n ; i += 2){
		if ((a[i].kind == UGLY_STRING)&&(a[i].as.s.len == strlen(key))&&(memcmp(a[i].as.s.p, key, a[i].as.s.len) == 0)){
			mval v ;
			ugly_status st = coerce(ctx, m, &a[i + 1], UGLY_INT, &v) ;
			if (st != UGLY_OK){
				return st ;
			}
			*out = v.as.i ;
		}
	}
	return UGLY_OK ;
}


/* kw(a, b => 0, c => 0): "a b c", the keyword fixture (§11). */
static ugly_status f_kw(ugly_ctx *ctx, mock_rt *m, mval *a, size_t n, mval *ret){
	if (n < 1){
		return raise(ctx, m, "ArgumentError", "kw takes a first argument") ;
	}
	mval first ;
	int64_t b, c ;
	ugly_status st = coerce(ctx, m, &a[0], UGLY_INT, &first) ;
	if ((st != UGLY_OK)||((st = pair_int(ctx, m, a, 1, n, "b", 0, &b)) != UGLY_OK)
		||((st = pair_int(ctx, m, a, 1, n, "c", 0, &c)) != UGLY_OK)){
		return st ;
	}
	char buf[96] ;
	int len = snprintf(buf, sizeof(buf), "%lld %lld %lld", (long long)first.as.i, (long long)b, (long long)c) ;
	*ret = mstr(UGLY_STRING, buf, (size_t)len) ;
	return UGLY_OK ;
}


static const struct { const char *name ; mock_fn fn ; } functions[] = {
	{ "kw", f_kw },
	{ "echo", f_echo },
	{ "kind_of", f_kind_of },
	{ "make_counter", f_make_counter },
	{ "apply", f_apply },
	{ "call_twice", f_call_twice },
	{ "raise_error", f_raise_error },
	{ "catch_and_return", f_catch_and_return },
	{ "rethrow", f_rethrow },
	{ "finalized", f_finalized },
	{ "inits", f_inits },
} ;
#define NFUNCTIONS (sizeof(functions) / sizeof(functions[0]))


/* The module is a list of members: one function object per fixture, plus the Point class. */
static mobj *module_new(mock_rt *m){
	mobj *mod = obj_new(m, M_MODULE) ;
	size_t i ;
	for (i = 0 ; i < NFUNCTIONS ; i++){
		mobj *f = obj_new(m, M_FUNC) ;
		f->name = functions[i].name ;
		f->fn = functions[i].fn ;
		list_push(mod, mobjval(f)) ;
	}
	mobj *cls = obj_new(m, M_CLASS) ;
	cls->name = "Point" ;
	list_push(mod, mobjval(cls)) ;
	cls = obj_new(m, M_CLASS) ;
	cls->name = "KwBox" ;
	list_push(mod, mobjval(cls)) ;
	return mod ;
}


/* ---------------------------------------------------------------- */
/* The plugin                                                       */
/* ---------------------------------------------------------------- */

static ugly_status mock_init(ugly_ctx *ctx, const char *dir){
	(void)ctx ; (void)dir ;
	/* Counted in the environment, which outlives dlclose: the restart test reads it. */
	const char *s = getenv("UGLY_MOCK_INITS") ;
	char buf[32] ;
	snprintf(buf, sizeof(buf), "%lld", (s != NULL ? atoll(s) : 0) + 1) ;
	setenv("UGLY_MOCK_INITS", buf, 1) ;
	return UGLY_OK ;
}


static void mock_term(void){
}


static ugly_status mock_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	(void)ctx ;
	mock_rt *m = calloc(1, sizeof(mock_rt)) ;
	if (m == NULL){
		return UGLY_ERR_NOMEM ;
	}
	m->urt = rt ;
	m->module = module_new(m) ;
	*native = m ;
	return UGLY_OK ;
}


static ugly_status mock_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	(void)ctx ; (void)rt ;
	mock_rt *m = native ;
	obj_unref(m->module) ;
	if (m->live != 0){
		fprintf(stderr, "mock: %lld object(s) still alive at runtime deletion\n", (long long)m->live) ;
	}
	free(m) ;
	return UGLY_OK ;
}


static mock_rt *M(ugly_rt *rt){
	return ugly_rt_native(rt) ;
}


static ugly_status mock_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out){
	mock_rt *m = M(rt) ;
	if (strcmp(name, "conformance") != 0){
		return raise(ctx, m, "ImportError", "no such module") ;
	}
	mval v = mobjval(m->module) ;
	return from_native(ctx, m, &v, UGLY_HANDLE, out) ;
}


static ugly_status mock_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_val *out){
	return mock_import(ctx, rt, "conformance", out) ;
}


static ugly_status mock_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name){
	(void)rt ;
	mobj *o = ref ;
	static const uint32_t t[] = {
		UGLY_TRAIT_MEMBERS,          /* module */
		UGLY_TRAIT_CALLABLE,         /* function */
		UGLY_TRAIT_MEMBERS,          /* counter */
		UGLY_TRAIT_ELEMENTS,         /* list */
		UGLY_TRAIT_INSTANTIABLE,     /* class */
		UGLY_TRAIT_MEMBERS,          /* point */
		UGLY_TRAIT_MEMBERS,          /* exception */
		0,                           /* proxy: never exported */
		UGLY_TRAIT_MEMBERS           /* kwbox */
	} ;
	*traits = t[o->kind] ;
	mval v = mobjval(o) ;
	const char *k = kind_of(&v) ;
	return ugly_val_set_string(ctx, type_name, k, strlen(k)) ;
}


/* A member of a native object, as a native value (a new reference). */
static int member(mobj *o, const char *name, mval *out){
	size_t i ;
	switch (o->kind){
		case M_MODULE:
			for (i = 0 ; i < o->n ; i++){
				if (strcmp(o->vals[i].as.o->name, name) == 0){
					*out = mval_copy(&o->vals[i]) ;
					return 1 ;
				}
			}
			return 0 ;
		case M_COUNTER:
			if (strcmp(name, "count") == 0){ *out = mint(o->count) ; return 1 ; }
			if (strcmp(name, "items") == 0){ o->items->refs++ ; *out = mobjval(o->items) ; return 1 ; }
			return 0 ;
		case M_POINT:
			if (strcmp(name, "x") == 0){ *out = mdouble(o->x) ; return 1 ; }
			if (strcmp(name, "y") == 0){ *out = mdouble(o->y) ; return 1 ; }
			return 0 ;
		case M_KWBOX:
			if (strcmp(name, "value") == 0){ *out = mint(o->count) ; return 1 ; }
			return 0 ;
		case M_EXC:
			if (strcmp(name, "type") == 0){ *out = mstr(UGLY_STRING, o->type, strlen(o->type)) ; return 1 ; }
			if (strcmp(name, "message") == 0){ *out = mstr(UGLY_STRING, o->message, strlen(o->message)) ; return 1 ; }
			return 0 ;
		default:
			return 0 ;
	}
}


static ugly_status mock_get_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want, ugly_val *out){
	mock_rt *m = M(rt) ;
	mval v ;
	if (! member(ref, name, &v)){
		return fail(ctx, m, UGLY_ERR_NOT_FOUND, "AttributeError", name) ;
	}
	ugly_status st = from_native(ctx, m, &v, want, out) ;
	mval_free(&v) ;
	return st ;
}


static ugly_status mock_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	mock_rt *m = M(rt) ;
	mobj *o = ref ;
	mval nv ;
	ugly_status st = to_native(ctx, m, v, &nv) ;
	if (st != UGLY_OK){
		return st ;
	}
	mval c ;
	if ((o->kind == M_COUNTER)&&(strcmp(name, "count") == 0)){
		st = coerce(ctx, m, &nv, UGLY_INT, &c) ;
		if (st == UGLY_OK){
			o->count = c.as.i ;
		}
	}
	else if ((o->kind == M_POINT)&&((strcmp(name, "x") == 0)||(strcmp(name, "y") == 0))){
		st = coerce(ctx, m, &nv, UGLY_DOUBLE, &c) ;
		if (st == UGLY_OK){
			*(name[0] == 'x' ? &o->x : &o->y) = c.as.d ;
		}
	}
	else {
		st = fail(ctx, m, UGLY_ERR_NOT_FOUND, "AttributeError", name) ;
	}
	mval_free(&nv) ;
	return st ;
}


/* Mock objects have fixed members. */
static ugly_status mock_delete_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name){
	(void)ref ; (void)name ;
	return fail(ctx, M(rt), UGLY_ERR_NOT_CAPABLE, "TypeError", "members can't be deleted") ;
}


static ugly_status mock_member_names(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	(void)rt ;
	mobj *o = ref ;
	ugly_val names[16] ;
	size_t n = 0, i ;
	switch (o->kind){
		case M_MODULE:
			for (i = 0 ; (i < o->n)&&(n < 16) ; i++){
				const char *s = o->vals[i].as.o->name ;
				names[n++] = ugly_str(s, strlen(s)) ;
			}
			break ;
		case M_COUNTER: names[n++] = ugly_str("count", 5) ; names[n++] = ugly_str("items", 5) ; break ;
		case M_KWBOX: names[n++] = ugly_str("value", 5) ; break ;
		case M_POINT: names[n++] = ugly_str("x", 1) ; names[n++] = ugly_str("y", 1) ; break ;
		case M_EXC: names[n++] = ugly_str("type", 4) ; names[n++] = ugly_str("message", 7) ; break ;
		default:
			return UGLY_ERR_NOT_CAPABLE ;
	}
	return ugly_list_new(ctx, names, n, out) ;
}


static ugly_status mock_length(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out){
	(void)ctx ; (void)rt ;
	mobj *o = ref ;
	if (o->kind != M_LIST){
		return UGLY_ERR_NOT_CAPABLE ;
	}
	*out = (int64_t)o->n ;
	return UGLY_OK ;
}


static ugly_status mock_get_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want, ugly_val *out){
	mock_rt *m = M(rt) ;
	mobj *o = ref ;
	if (o->kind != M_LIST){
		return UGLY_ERR_NOT_CAPABLE ;
	}
	if ((i < 0)||((uint64_t)i >= o->n)){
		return fail(ctx, m, UGLY_ERR_NOT_FOUND, "IndexError", "index out of range") ;
	}
	return from_native(ctx, m, &o->vals[i], want, out) ;
}


static ugly_status mock_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	mock_rt *m = M(rt) ;
	mobj *o = ref ;
	if (o->kind != M_LIST){
		return UGLY_ERR_NOT_CAPABLE ;
	}
	if ((i < 0)||((uint64_t)i > o->n)){
		return fail(ctx, m, UGLY_ERR_NOT_FOUND, "IndexError", "index out of range") ;
	}
	mval nv ;
	ugly_status st = to_native(ctx, m, v, &nv) ;
	if (st != UGLY_OK){
		return st ;
	}
	if ((uint64_t)i == o->n){
		list_push(o, nv) ;
	}
	else {
		mval_free(&o->vals[i]) ;
		o->vals[i] = nv ;
	}
	return UGLY_OK ;
}


static ugly_status mock_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n){

	mock_rt *m = M(rt) ;
	mobj *o = ref ;
	if (o->kind != M_LIST){
		return UGLY_ERR_NOT_CAPABLE ;
	}
	size_t off = (size_t)offset, rm = (size_t)remove, i ;
	mval *nv = calloc(n + 1, sizeof(mval)) ;
	for (i = 0 ; i < n ; i++){
		ugly_status st = to_native(ctx, m, insert[i], &nv[i]) ;
		if (st != UGLY_OK){
			while (i-- > 0){
				mval_free(&nv[i]) ;
			}
			free(nv) ;
			return st ;
		}
	}
	for (i = 0 ; i < rm ; i++){
		mval_free(&o->vals[off + i]) ;
	}
	size_t nn = o->n - rm + n ;
	if (nn > o->cap){
		o->cap = nn ;
		o->vals = realloc(o->vals, o->cap * sizeof(mval)) ;
	}
	memmove(&o->vals[off + n], &o->vals[off + rm], (o->n - off - rm) * sizeof(mval)) ;
	memcpy(&o->vals[off], nv, n * sizeof(mval)) ;
	o->n = nn ;
	free(nv) ;
	return UGLY_OK ;
}


/* Converts borrowed arguments, runs fn, and converts the result. */
static ugly_status run(ugly_ctx *ctx, mock_rt *m, ugly_status (*fn)(ugly_ctx *, mock_rt *, mobj *, mval *, size_t, mval *),
	mobj *self, const ugly_val *args, size_t nargs, ugly_kind want, ugly_val *out){

	mval *a = calloc(nargs + 1, sizeof(mval)) ;
	ugly_status st = UGLY_OK ;
	size_t i, made = 0 ;
	for (i = 0 ; (i < nargs)&&(st == UGLY_OK) ; i++, made++){
		st = to_native(ctx, m, args[i], &a[i]) ;
	}
	mval ret = mnull() ;
	if (st == UGLY_OK){
		st = fn(ctx, m, self, a, nargs, &ret) ;
	}
	if (st == UGLY_OK){
		st = from_native(ctx, m, &ret, want, out) ;
	}
	mval_free(&ret) ;
	for (i = 0 ; i < made ; i++){
		mval_free(&a[i]) ;
	}
	free(a) ;
	return st ;
}


static ugly_status call_func(ugly_ctx *ctx, mock_rt *m, mobj *self, mval *a, size_t n, mval *ret){
	return self->fn(ctx, m, a, n, ret) ;
}


static ugly_status mock_call_op(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out){

	mobj *o = ref ;
	if (o->kind != M_FUNC){
		return fail(ctx, M(rt), UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not callable") ;
	}
	return run(ctx, M(rt), call_func, o, args, nargs, want, out) ;
}


/* Methods: counter.inc(), point.norm2(), and module.f(...) (calls the member, like Lua's '.'). */
static ugly_status call_method(ugly_ctx *ctx, mock_rt *m, mobj *self, mval *a, size_t n, mval *ret) ;
static _Thread_local const char *current_method ;

static ugly_status mock_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out){

	const char *saved = current_method ;     /* invokes nest: restore on the way out */
	current_method = method ;
	ugly_status st = run(ctx, M(rt), call_method, ref, args, nargs, want, out) ;
	current_method = saved ;
	return st ;
}


static ugly_status call_method(ugly_ctx *ctx, mock_rt *m, mobj *self, mval *a, size_t n, mval *ret){
	const char *method = current_method ;
	if ((self->kind == M_COUNTER)&&(strcmp(method, "inc") == 0)){
		ARGS(0) ;
		self->count++ ;
		list_push(self->items, mint(self->count)) ;
		*ret = mint(self->count) ;
		return UGLY_OK ;
	}
	if ((self->kind == M_KWBOX)&&(strcmp(method, "add") == 0)){
		int64_t k ;
		ugly_status st = pair_int(ctx, m, a, 0, n, "n", 1, &k) ;
		if (st == UGLY_OK){
			*ret = mint(self->count + k) ;
		}
		return st ;
	}
	if ((self->kind == M_POINT)&&(strcmp(method, "norm2") == 0)){
		ARGS(0) ;
		*ret = mdouble(self->x * self->x + self->y * self->y) ;
		return UGLY_OK ;
	}
	mval f ;
	if ((self->kind == M_MODULE)&&(member(self, method, &f))){
		ugly_status st = mock_call(ctx, m, &f, a, n, ret) ;
		mval_free(&f) ;
		return st ;
	}
	return fail(ctx, m, UGLY_ERR_NOT_FOUND, "AttributeError", method) ;
}


static ugly_status new_point(ugly_ctx *ctx, mock_rt *m, mobj *self, mval *a, size_t n, mval *ret){
	(void)self ;
	ARGS(2) ;
	mval x, y ;
	ugly_status st = coerce(ctx, m, &a[0], UGLY_DOUBLE, &x) ;
	if (st == UGLY_OK){
		st = coerce(ctx, m, &a[1], UGLY_DOUBLE, &y) ;
	}
	if (st != UGLY_OK){
		return st ;
	}
	mobj *p = obj_new(m, M_POINT) ;
	p->x = x.as.d ;
	p->y = y.as.d ;
	*ret = mobjval(p) ;
	return UGLY_OK ;
}


/* KwBox(value => 0): the keyword fixture's class; box.add(n => 1) is value + n. */
static ugly_status new_kwbox(ugly_ctx *ctx, mock_rt *m, mobj *self, mval *a, size_t n, mval *ret){
	(void)self ;
	int64_t value ;
	ugly_status st = pair_int(ctx, m, a, 0, n, "value", 0, &value) ;
	if (st != UGLY_OK){
		return st ;
	}
	mobj *b = obj_new(m, M_KWBOX) ;
	b->count = value ;
	*ret = mobjval(b) ;
	return UGLY_OK ;
}


static ugly_status mock_new_instance(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_handle *out){

	mobj *o = ref ;
	if (o->kind != M_CLASS){
		return UGLY_ERR_NOT_CAPABLE ;
	}
	ugly_val v = ugly_null() ;
	ugly_status st = run(ctx, M(rt), (strcmp(o->name, "KwBox") == 0 ? new_kwbox : new_point), o, args, nargs,
		UGLY_HANDLE, &v) ;
	if (st == UGLY_OK){
		*out = v.as.h ;
	}
	return st ;
}


/*
 * Call options: keyword arguments become trailing name, value pairs (as in
 * Perl); a hint or a context is refused (contract §11: exact or refused).
 */
static ugly_val *with_pairs(ugly_ctx *ctx, ugly_rt *rt, const ugly_val *args, size_t nargs, const ugly_opts *o,
	size_t *total, ugly_status *st){

	*st = UGLY_OK ;
	if ((o->hint != NULL)||(o->context != NULL)){
		*st = fail(ctx, M(rt), UGLY_ERR_NOT_SUPPORTED, "ArgumentError",
			o->hint != NULL ? "the mock takes no call hint" : "the mock takes no call context") ;
		return NULL ;
	}
	*total = nargs + 2 * o->nkwargs ;
	ugly_val *all = calloc(*total + 1, sizeof(ugly_val)) ;
	if (all == NULL){
		*st = UGLY_ERR_NOMEM ;
		return NULL ;
	}
	memcpy(all, args, nargs * sizeof(ugly_val)) ;
	size_t i ;
	for (i = 0 ; i < o->nkwargs ; i++){
		all[nargs + 2 * i] = ugly_str(o->kwargs[i].name, strlen(o->kwargs[i].name)) ;
		all[nargs + 2 * i + 1] = o->kwargs[i].value ;
	}
	return all ;
}


static ugly_status mock_call_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_kind want, ugly_val *out){

	size_t n ;
	ugly_status st ;
	ugly_val *all = with_pairs(ctx, rt, args, nargs, o, &n, &st) ;
	if (all != NULL){
		st = mock_call_op(ctx, rt, ref, all, n, want, out) ;
		free(all) ;
	}
	return st ;
}


static ugly_status mock_invoke_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, const ugly_opts *o, ugly_kind want, ugly_val *out){

	size_t n ;
	ugly_status st ;
	ugly_val *all = with_pairs(ctx, rt, args, nargs, o, &n, &st) ;
	if (all != NULL){
		st = mock_invoke(ctx, rt, ref, method, all, n, want, out) ;
		free(all) ;
	}
	return st ;
}


static ugly_status mock_new_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_handle *out){

	size_t n ;
	ugly_status st ;
	ugly_val *all = with_pairs(ctx, rt, args, nargs, o, &n, &st) ;
	if (all != NULL){
		st = mock_new_instance(ctx, rt, ref, all, n, out) ;
		free(all) ;
	}
	return st ;
}


static void mock_release(ugly_rt *rt, void *ref){
	(void)rt ;
	obj_unref(ref) ;
}


static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = UGLY_CAP_MULTI_RUNTIME | UGLY_CAP_RESTARTABLE,
	.name = "mock",
	.init = mock_init,
	.term = mock_term,
	.rt_new = mock_rt_new,
	.rt_delete = mock_rt_delete,
	.import = mock_import,
	.globals = mock_globals,
	.describe = mock_describe,
	.get_member = mock_get_member,
	.set_member = mock_set_member,
	.member_names = mock_member_names,
	.length = mock_length,
	.get_index = mock_get_index,
	.set_index = mock_set_index,
	.call_opts = mock_call_opts,
	.invoke_opts = mock_invoke_opts,
	.new_opts = mock_new_opts,
	.delete_member = mock_delete_member,
	.splice = mock_splice,
	.call = mock_call_op,
	.invoke = mock_invoke,
	.new_instance = mock_new_instance,
	.release = mock_release,
} ;


const ugly_plugin *ugly_plugin_mock(void){
	return &plugin ;
}
