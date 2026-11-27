/*
 * The JavaScript plugin, on QuickJS. Each runtime is a JSRuntime with one
 * JSContext.
 *
 * - Native refs are boxed JSValues (one reference each). Identity is the
 *   object's address.
 * - Numbers: JS has one number type, so an integral number within +-2^53
 *   comes out as INT, and other numbers as DOUBLE. INTs beyond 2^53 go in as
 *   BigInt (so int64 round-trips), and a BigInt comes out as INT if it fits.
 *   Bytes are ArrayBuffers.
 * - Foreign values are objects of two classes, proxy (members, elements) and
 *   proxy function (also call/new), with exotic property handlers. They're
 *   cached per handle, so the same foreign value is the same object (===).
 *   A callable member is returned as a method bound to the proxy, so
 *   proxy.m(x) invokes m with the foreign language's receiver rules.
 * - A ugly_error raised into JS is an object inheriting Error, with type,
 *   message, language and payload. It's restored unchanged if it escapes. If
 *   the payload is a JS value coming home, that value itself is thrown
 *   (rule 7).
 * - Modules are ES modules, loaded by name from UGLY_JS_PATH (colon-separated
 *   directories, "<name>.js"). "ugly" is a built-in module: module(), eval(),
 *   isError(), isProxy(), gc().
 * - Pending jobs (promises, FinalizationRegistry callbacks) run when the
 *   outermost call into the runtime returns.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "ugly_plugin.h"


#define MAX_SAFE_INT 9007199254740991.0     /* 2^53 - 1 */

typedef struct pcache_entry {
	ugly_handle h ;
	void *obj ;                  /* JSObject*, not a reference: removed by the proxy's finalizer */
	struct pcache_entry *next ;
} pcache_entry ;

/* A native ref: one JSValue reference held for a handle, linked per runtime. */
typedef struct jsbox {
	JSValue v ;
	int open ;                   /* an iterator stepped (ugly_next) and not done: release calls its return() */
	int is_class ;               /* a constructor: whether it's a class (is_class); -1 until asked */
	struct jsbox *prev, *next ;
} jsbox ;

typedef struct jsrt {
	struct jsrt *next ;          /* the registry of live runtimes */
	jsbox *boxes ;               /* every native ref held for a handle */
	ugly_rt *urt ;
	JSRuntime *rt ;
	JSContext *c ;
	int owns ;                   /* created by rt_new; attached ones belong to the host */
	int depth ;                  /* our entries in progress: jobs run when the outermost returns */
	JSValue error_proto ;        /* prototype of ugly errors (inherits Error.prototype) */
	JSValue array_buffer ;       /* the ArrayBuffer constructor */
	JSAtom to_string_tag ;       /* Symbol.toStringTag */
	JSAtom iterator ;            /* Symbol.iterator */
	pcache_entry *pcache[64] ;
} jsrt ;

typedef struct jsproxy {
	jsrt *r ;
	ugly_handle h ;              /* one owned reference */
	uint32_t traits ;
} jsproxy ;

typedef struct { ugly_error *err ; } errbox ;

static JSClassID proxy_class, proxy_fn_class, error_class, kw_class, opts_class ;


/*
 * Live runtimes, created or attached. A lookup by context, rather than
 * the context opaque, because an attached context belongs to the host. Proxy
 * finalizers check it too, since a proxy can outlive a detached runtime.
 */
static jsrt *runtimes ;


static jsrt *R(JSContext *c){
	jsrt *r ;
	for (r = runtimes ; (r != NULL)&&(r->c != c) ; r = r->next){
	}
	return r ;
}


static int registered(jsrt *r){
	jsrt *x ;
	for (x = runtimes ; (x != NULL)&&(x != r) ; x = x->next){
	}
	return (x != NULL) ;
}


static void unregister(jsrt *r){
	jsrt **p ;
	for (p = &runtimes ; *p != NULL ; p = &(*p)->next){
		if (*p == r){
			*p = r->next ;
			return ;
		}
	}
}


/* ---------------------------------------------------------------- */
/* Small helpers                                                    */
/* ---------------------------------------------------------------- */

static int valid_utf8(const unsigned char *s, size_t len){
	size_t i = 0 ;
	while (i < len){
		unsigned char c = s[i] ;
		size_t n ;
		uint32_t cp ;
		if (c < 0x80){ i++ ; continue ; }
		else if ((c & 0xE0) == 0xC0){ n = 1 ; cp = c & 0x1F ; }
		else if ((c & 0xF0) == 0xE0){ n = 2 ; cp = c & 0x0F ; }
		else if ((c & 0xF8) == 0xF0){ n = 3 ; cp = c & 0x07 ; }
		else { return 0 ; }
		if (i + n >= len){
			return 0 ;
		}
		size_t k ;
		for (k = 1 ; k <= n ; k++){
			if ((s[i + k] & 0xC0) != 0x80){
				return 0 ;
			}
			cp = (cp << 6) | (s[i + k] & 0x3F) ;
		}
		if ((n == 1 && cp < 0x80)||(n == 2 && cp < 0x800)||(n == 3 && cp < 0x10000)||
			(cp > 0x10FFFF)||((cp >= 0xD800)&&(cp <= 0xDFFF))){
			return 0 ;     /* overlong, out of range, or a (lone) surrogate */
		}
		i += n + 1 ;
	}
	return 1 ;
}


static jsproxy *proxy_of(JSValueConst v){
	jsproxy *p = JS_GetOpaque(v, proxy_class) ;
	return (p != NULL ? p : JS_GetOpaque(v, proxy_fn_class)) ;
}


static int is_array_buffer(jsrt *r, JSValueConst v){
	return JS_IsObject(v) && (JS_IsInstanceOf(r->c, v, r->array_buffer) > 0) ;
}


static void drop_exception(JSContext *c){
	JS_FreeValue(c, JS_GetException(c)) ;
}


static void run_jobs(jsrt *r){
	JSContext *jc ;
	while (JS_IsJobPending(r->rt)){
		if (JS_ExecutePendingJob(r->rt, &jc) < 0){
			drop_exception(jc) ;     /* an unhandled rejection in a job has nowhere to go */
		}
	}
}


/*
 * Releases made from a QuickJS finalizer. A finalizer runs inside the
 * engine's GC, where no JS may run, and releasing a foreign value can run
 * anything: the value's destructor (a Perl DESTROY, a Lua __gc) may call
 * back into JS, and a JS iterator it held is closed by calling its return()
 * (js_release). So a finalizer only queues what it has to release, when
 * something on this thread will run the queue (will_run > 0: inside one of
 * our entries, during rt_delete, while the binding is attached), at a point
 * where JS may run: a call out from JS (need_ctx), leave(), the end of
 * rt_delete and of the binding's teardown. Otherwise no runtime of ours is
 * left on this thread to re-enter, and it releases on the spot.
 */
typedef struct { ugly_handle h ; ugly_error *err ; } deferred ;
static _Thread_local deferred *later ;
static _Thread_local size_t nlater, later_cap ;
static _Thread_local int will_run ;

static void release_later(ugly_handle h, ugly_error *err){
	if (will_run > 0){
		if (nlater == later_cap){
			size_t ncap = (later_cap ? later_cap * 2 : 16) ;
			deferred *nl = realloc(later, ncap * sizeof(deferred)) ;
			if (nl != NULL){
				later = nl ;
				later_cap = ncap ;
			}
		}
		if (nlater < later_cap){
			later[nlater].h = h ;
			later[nlater++].err = err ;
			return ;
		}
	}
	ugly_release(ugly_current_ctx(), h) ;
	ugly_error_free(err) ;
}


/* Runs the queue, and what running it queues. Keeps the ctx's error: a release is cleanup. */
static void run_later(void){
	if (nlater == 0){
		return ;
	}
	ugly_ctx *ctx = ugly_current_ctx() ;
	ugly_error *kept = (ctx != NULL ? ugly_take_error(ctx) : NULL) ;
	while (nlater > 0){
		deferred *batch = later ;
		size_t n = nlater, i ;
		later = NULL ;
		nlater = later_cap = 0 ;
		for (i = 0 ; i < n ; i++){
			ugly_release(ctx, batch[i].h) ;
			ugly_error_free(batch[i].err) ;
		}
		free(batch) ;
	}
	if (kept != NULL){
		ugly_restore_error(ctx, kept) ;
	}
}


static void enter(jsrt *r){
	will_run++ ;
	/*
	 * QuickJS checks for stack overflow against the stack top it recorded
	 * for this runtime. A host may run it on another thread, one at a time
	 * (ugly_rt_set_host_serialized): the outermost entry records this
	 * thread's.
	 */
	if (r->depth++ == 0){
		JS_UpdateStackTop(JS_GetRuntime(r->c)) ;
	}
}


/* Pending jobs run when the outermost call returns, unless the host owns the event loop (attached). */
static ugly_status leave(jsrt *r, ugly_status st){
	if ((--r->depth == 0)&&(r->owns)&&JS_IsJobPending(r->rt)){
		/* A job may call out, which would erase the error of the call that just failed. */
		ugly_ctx *ctx = ugly_current_ctx() ;
		ugly_error *kept = (ctx != NULL ? ugly_take_error(ctx) : NULL) ;
		run_jobs(r) ;
		if (kept != NULL){
			ugly_restore_error(ctx, kept) ;
		}
	}
	run_later() ;
	will_run-- ;
	return st ;
}


static ugly_status fail(ugly_ctx *ctx, jsrt *r, ugly_status st, const char *type, const char *msg){
	return ugly_fail(ctx, r->urt, st, type, msg) ;
}


/* ---------------------------------------------------------------- */
/* The proxy cache: owned handle -> proxy object                    */
/* ---------------------------------------------------------------- */

static size_t pc_bucket(ugly_handle h){
	return (size_t)((h * 0x9E3779B97F4A7C15ull) >> 58) ;    /* 64 buckets */
}


static void *pc_find(jsrt *r, ugly_handle h){
	pcache_entry *e ;
	for (e = r->pcache[pc_bucket(h)] ; e != NULL ; e = e->next){
		if (e->h == h){
			return e->obj ;
		}
	}
	return NULL ;
}


static void pc_add(jsrt *r, ugly_handle h, void *obj){
	pcache_entry *e = malloc(sizeof(pcache_entry)) ;
	if (e == NULL){
		return ;
	}
	e->h = h ;
	e->obj = obj ;
	e->next = r->pcache[pc_bucket(h)] ;
	r->pcache[pc_bucket(h)] = e ;
}


static void pc_remove(jsrt *r, ugly_handle h, void *obj){
	pcache_entry **p ;
	for (p = &r->pcache[pc_bucket(h)] ; *p != NULL ; p = &(*p)->next){
		if (((*p)->h == h)&&((*p)->obj == obj)){
			pcache_entry *e = *p ;
			*p = e->next ;
			free(e) ;
			return ;
		}
	}
}


/* ---------------------------------------------------------------- */
/* JS -> ugly_val                                                   */
/* ---------------------------------------------------------------- */

/* A box for a JS value reference (consumed). */
static jsbox *new_box(jsrt *r, JSValue v){
	jsbox *b = malloc(sizeof(jsbox)) ;
	if (b == NULL){
		JS_FreeValueRT(r->rt, v) ;
		return NULL ;
	}
	b->v = v ;
	b->open = 0 ;
	b->is_class = -1 ;
	b->prev = NULL ;
	b->next = r->boxes ;
	if (r->boxes != NULL){
		r->boxes->prev = b ;
	}
	r->boxes = b ;
	return b ;
}


static void free_box(jsrt *r, jsbox *b){
	if (b->prev != NULL){
		b->prev->next = b->next ;
	}
	else {
		r->boxes = b->next ;
	}
	if (b->next != NULL){
		b->next->prev = b->prev ;
	}
	JS_FreeValueRT(r->rt, b->v) ;
	free(b) ;
}


/* Exports a JS value as an owned handle; our own proxies unwrap to their handle (rule 5). */
static ugly_status export_val(ugly_ctx *ctx, jsrt *r, JSValueConst v, ugly_handle *out){
	jsproxy *p = proxy_of(v) ;
	if (p != NULL){
		return ugly_dup(ctx, p->h, out) ;
	}
	jsbox *box = new_box(r, JS_DupValue(r->c, v)) ;
	if (box == NULL){
		return UGLY_ERR_NOMEM ;
	}
	uintptr_t identity = (JS_VALUE_HAS_REF_COUNT(v) ? (uintptr_t)JS_VALUE_GET_PTR(v) : 0) ;
	return ugly_export(ctx, r->urt, box, identity, out) ;
}


static ugly_status type_error(ugly_ctx *ctx, jsrt *r, const char *to){
	char msg[96] ;
	snprintf(msg, sizeof(msg), "can't convert this JS value to %s", to) ;
	return fail(ctx, r, UGLY_ERR_TYPE, "TypeError", msg) ;
}


static ugly_status js_failure(ugly_ctx *ctx, jsrt *r) ;

/* A JS value as a result of kind 'want', by JS's own rules (lenient where JS itself is: number -> string). */
static ugly_status to_val(ugly_ctx *ctx, jsrt *r, JSValueConst v, ugly_kind want, ugly_val *out){
	JSContext *c = r->c ;
	int tag = JS_VALUE_GET_NORM_TAG(v) ;
	int is_bigint = JS_IsBigInt(c, v) ;
	*out = ugly_null() ;

	if (want == UGLY_ANY){
		if ((tag == JS_TAG_NULL)||(tag == JS_TAG_UNDEFINED)){
			return UGLY_OK ;
		}
		else if (tag == JS_TAG_BOOL){
			want = UGLY_BOOL ;
		}
		else if (tag == JS_TAG_INT){
			want = UGLY_INT ;
		}
		else if (tag == JS_TAG_FLOAT64){
			double d = JS_VALUE_GET_FLOAT64(v) ;
			want = ((isfinite(d))&&(d == floor(d))&&(fabs(d) <= MAX_SAFE_INT)&&(! ((d == 0)&&(signbit(d))))
				? UGLY_INT : UGLY_DOUBLE) ;
		}
		else if (is_bigint){
			want = UGLY_INT ;
		}
		else if (JS_IsString(v)){
			want = UGLY_STRING ;
		}
		else if (is_array_buffer(r, v)){
			want = UGLY_BYTES ;
		}
		else {
			want = UGLY_HANDLE ;
		}
	}

	switch (want){
		case UGLY_NULL:
			return (((tag == JS_TAG_NULL)||(tag == JS_TAG_UNDEFINED)) ? UGLY_OK : type_error(ctx, r, "null")) ;
		case UGLY_BOOL: {
			int b = JS_ToBool(c, v) ;     /* JS truthiness */
			if (b < 0){
				return js_failure(ctx, r) ;
			}
			*out = ugly_bool(b) ;
			return UGLY_OK ;
		}
		case UGLY_INT: {
			if (tag == JS_TAG_INT){
				*out = ugly_int(JS_VALUE_GET_INT(v)) ;
				return UGLY_OK ;
			}
			if (tag == JS_TAG_FLOAT64){
				double d = JS_VALUE_GET_FLOAT64(v) ;
				if ((d == floor(d))&&(d >= -9223372036854775808.0)&&(d < 9223372036854775808.0)){
					*out = ugly_int((int64_t)d) ;
					return UGLY_OK ;
				}
				return type_error(ctx, r, "an integer") ;
			}
			if (is_bigint){
				/* JS_ToBigInt64 wraps around: check that the value really fits. */
				int64_t i ;
				if (JS_ToBigInt64(c, &i, v) < 0){
					return js_failure(ctx, r) ;
				}
				const char *s = JS_ToCString(c, v) ;
				char buf[32] ;
				snprintf(buf, sizeof(buf), "%lld", (long long)i) ;
				int fits = ((s != NULL)&&(strcmp(s, buf) == 0)) ;
				JS_FreeCString(c, s) ;
				if (! fits){
					return fail(ctx, r, UGLY_ERR_TYPE, "RangeError", "BigInt doesn't fit in int64") ;
				}
				*out = ugly_int(i) ;
				return UGLY_OK ;
			}
			return type_error(ctx, r, "an integer") ;     /* no implicit string -> number */
		}
		case UGLY_DOUBLE: {
			if (! JS_IsNumber(v)){
				return type_error(ctx, r, "a double") ;
			}
			double d ;
			JS_ToFloat64(c, &d, v) ;
			*out = ugly_double(d) ;
			return UGLY_OK ;
		}
		case UGLY_STRING: {
			if ((! JS_IsString(v))&&(! JS_IsNumber(v))&&(! is_bigint)){
				return type_error(ctx, r, "a string") ;     /* numbers stringify, like "" + n */
			}
			size_t len ;
			const char *s = JS_ToCStringLen(c, &len, v) ;
			if (s == NULL){
				return js_failure(ctx, r) ;
			}
			ugly_status st = (valid_utf8((const unsigned char *)s, len)
				? ugly_val_set_string(ctx, out, s, len)
				: fail(ctx, r, UGLY_ERR_TYPE, "TypeError", "string has a lone surrogate")) ;
			JS_FreeCString(c, s) ;
			return st ;
		}
		case UGLY_BYTES: {
			size_t len ;
			uint8_t *p = (is_array_buffer(r, v) ? JS_GetArrayBuffer(c, &len, v) : NULL) ;
			if (p == NULL){
				if (JS_HasException(c)){
					drop_exception(c) ;     /* a detached buffer */
				}
				return type_error(ctx, r, "bytes") ;
			}
			return ugly_val_set_bytes(ctx, out, p, len) ;
		}
		case UGLY_HANDLE: {
			if (! JS_VALUE_HAS_REF_COUNT(v)||JS_IsString(v)||is_bigint){
				return type_error(ctx, r, "a handle") ;
			}
			ugly_handle h ;
			ugly_status st = export_val(ctx, r, v, &h) ;
			if (st == UGLY_OK){
				*out = ugly_hval(h) ;
			}
			return st ;
		}
		default:
			return UGLY_ERR_USAGE ;
	}
}


/* ---------------------------------------------------------------- */
/* ugly_val -> JS                                                   */
/* ---------------------------------------------------------------- */

static JSValue throw_ugly(JSContext *c, ugly_ctx *ctx, ugly_status st) ;

/* A proxy for a foreign handle: one per value (cached), owning one reference. */
static JSValue proxy_for(ugly_ctx *ctx, jsrt *r, ugly_handle borrowed){
	JSContext *c = r->c ;
	ugly_handle h ;
	ugly_status st = ugly_dup(ctx, borrowed, &h) ;     /* the owned handle: the same id for the same value */
	if (st != UGLY_OK){
		return throw_ugly(c, ctx, st) ;
	}
	void *cached = pc_find(r, h) ;
	if (cached != NULL){
		ugly_release(ctx, h) ;     /* the cached proxy holds its own reference */
		return JS_DupValue(c, JS_MKPTR(JS_TAG_OBJECT, cached)) ;
	}
	uint32_t traits = 0 ;
	ugly_val tn ;
	if (ugly_describe(ctx, h, &traits, &tn) == UGLY_OK){
		ugly_val_release(ctx, &tn) ;
	}
	int fn = ((traits & (UGLY_TRAIT_CALLABLE | UGLY_TRAIT_INSTANTIABLE)) != 0) ;
	JSValue obj = JS_NewObjectClass(c, (int)(fn ? proxy_fn_class : proxy_class)) ;
	jsproxy *p = malloc(sizeof(jsproxy)) ;
	if (JS_IsException(obj)||(p == NULL)){
		free(p) ;
		ugly_release(ctx, h) ;
		return (JS_IsException(obj) ? obj : JS_ThrowOutOfMemory(c)) ;
	}
	p->r = r ;
	p->h = h ;
	p->traits = traits ;
	JS_SetOpaque(obj, p) ;
	if (traits & UGLY_TRAIT_INSTANTIABLE){
		JS_SetConstructorBit(c, obj, 1) ;
	}
	pc_add(r, h, JS_VALUE_GET_PTR(obj)) ;
	return obj ;
}


/* An argument (borrowed) as a new JS value: ours resolve to the original (rule 5). JS_EXCEPTION on failure. */
static JSValue from_val(ugly_ctx *ctx, jsrt *r, ugly_val v){
	JSContext *c = r->c ;
	switch (v.kind){
		case UGLY_NULL: return JS_NULL ;
		case UGLY_BOOL: return JS_NewBool(c, v.as.b) ;
		case UGLY_INT:
			return ((v.as.i >= -(int64_t)MAX_SAFE_INT)&&(v.as.i <= (int64_t)MAX_SAFE_INT)
				? JS_NewInt64(c, v.as.i) : JS_NewBigInt64(c, v.as.i)) ;
		case UGLY_DOUBLE: return JS_NewFloat64(c, v.as.d) ;
		case UGLY_STRING: return JS_NewStringLen(c, v.as.str.ptr, v.as.str.len) ;
		case UGLY_BYTES: return JS_NewArrayBufferCopy(c, v.as.bytes.ptr, v.as.bytes.len) ;
		case UGLY_HANDLE: {
			void *ref = NULL ;
			ugly_status st = ugly_resolve(ctx, r->urt, v.as.h, &ref) ;
			if (st != UGLY_OK){
				return throw_ugly(c, ctx, st) ;
			}
			if (ref != NULL){
				return JS_DupValue(c, ((jsbox *)ref)->v) ;
			}
			return proxy_for(ctx, r, v.as.h) ;
		}
		default:
			return JS_ThrowTypeError(c, "ugly: invalid value") ;
	}
}


/* ---------------------------------------------------------------- */
/* Errors at the boundary                                           */
/* ---------------------------------------------------------------- */

/*
 * Throws the ctx's current error in JS: the original value if its payload
 * is ours coming home (rule 7), otherwise an ugly error object. Returns
 * JS_EXCEPTION.
 */
static JSValue throw_ugly(JSContext *c, ugly_ctx *ctx, ugly_status st){
	jsrt *r = R(c) ;
	if (ugly_last_error(ctx) == NULL){
		ugly_fail(ctx, r->urt, st, "Error", "operation failed") ;
	}
	ugly_error *err = ugly_take_error(ctx) ;
	if (err == NULL){
		return JS_ThrowInternalError(c, "ugly: operation failed") ;
	}
	ugly_handle payload = UGLY_NO_HANDLE ;
	ugly_error_payload(ctx, err, &payload) ;
	void *ref = NULL ;
	if ((payload != UGLY_NO_HANDLE)&&(ugly_resolve(ctx, r->urt, payload, &ref) == UGLY_OK)&&(ref != NULL)){
		/* Take our reference first: releasing the error may free the box 'ref' points to. */
		JSValue original = JS_DupValue(c, ((jsbox *)ref)->v) ;
		ugly_release(ctx, payload) ;
		ugly_error_free(err) ;
		return JS_Throw(c, original) ;
	}

	JSValue e = JS_NewObjectProtoClass(c, r->error_proto, error_class) ;
	errbox *b = malloc(sizeof(errbox)) ;
	if (JS_IsException(e)||(b == NULL)){
		free(b) ;
		ugly_release(ctx, payload) ;
		ugly_error_free(err) ;
		return (JS_IsException(e) ? e : JS_ThrowOutOfMemory(c)) ;
	}
	b->err = err ;
	JS_SetOpaque(e, b) ;
	JS_DefinePropertyValueStr(c, e, "name", JS_NewString(c, ugly_error_type(err)), JS_PROP_C_W_E) ;
	JS_DefinePropertyValueStr(c, e, "type", JS_NewString(c, ugly_error_type(err)), JS_PROP_C_W_E) ;
	JS_DefinePropertyValueStr(c, e, "message", JS_NewString(c, ugly_error_message(err)), JS_PROP_C_W_E) ;
	JS_DefinePropertyValueStr(c, e, "language", JS_NewString(c, ugly_error_language(err)), JS_PROP_C_W_E) ;
	JSValue pv = JS_UNDEFINED ;
	if (payload != UGLY_NO_HANDLE){
		pv = from_val(ctx, r, ugly_hval(payload)) ;
		if (JS_IsException(pv)){
			drop_exception(c) ;
			pv = JS_UNDEFINED ;
		}
		ugly_release(ctx, payload) ;
	}
	JS_DefinePropertyValueStr(c, e, "payload", pv, JS_PROP_C_W_E) ;
	return JS_Throw(c, e) ;
}


/* A string property of an object, copied into buf (empty if absent). */
static void str_prop(JSContext *c, JSValueConst obj, const char *name, char *buf, size_t size){
	buf[0] = '\0' ;
	JSValue v = JS_GetPropertyStr(c, obj, name) ;
	if (JS_IsException(v)){
		drop_exception(c) ;
		return ;
	}
	if (JS_IsString(v)){
		const char *s = JS_ToCString(c, v) ;
		if (s != NULL){
			snprintf(buf, size, "%s", s) ;
			JS_FreeCString(c, s) ;
		}
	}
	JS_FreeValue(c, v) ;
}


/* The JS exception that is pending, escaping to the core (rule 7). Clears it. */
static ugly_status js_failure(ugly_ctx *ctx, jsrt *r){
	JSContext *c = r->c ;
	JSValue e = JS_GetException(c) ;

	/* Our own error object, not yet consumed: the original error, unchanged. */
	errbox *b = JS_GetOpaque(e, error_class) ;
	if ((b != NULL)&&(b->err != NULL)){
		ugly_error *err = b->err ;
		b->err = NULL ;
		JS_FreeValue(c, e) ;
		return ugly_restore_error(ctx, err) ;
	}

	/* A rethrown foreign value: the payload is that value itself. */
	jsproxy *p = proxy_of(e) ;
	if (p != NULL){
		ugly_handle h ;
		ugly_status st = ugly_dup(ctx, p->h, &h) ;
		JS_FreeValue(c, e) ;
		return ugly_raise_handle(ctx, r->urt, "ForeignError", "rethrown foreign value", (st == UGLY_OK ? h : UGLY_NO_HANDLE)) ;
	}

	/* A JS exception: an Error's name and message, or the thrown value itself. */
	char type[96] = "error" ;
	const char *text = NULL ;     /* the message, whole: JS's own string, which the core copies */
	if (JS_IsError(c, e)){
		str_prop(c, e, "name", type, sizeof(type)) ;
		JSValue m = JS_GetPropertyStr(c, e, "message") ;
		text = (JS_IsString(m) ? JS_ToCString(c, m) : NULL) ;
		if (JS_IsException(m)){
			drop_exception(c) ;
		}
		JS_FreeValue(c, m) ;
		if (type[0] == '\0'){
			snprintf(type, sizeof(type), "Error") ;
		}
	}
	else {
		text = JS_ToCString(c, e) ;
		if (text == NULL){
			drop_exception(c) ;
		}
		if (! JS_IsString(e)){
			snprintf(type, sizeof(type), "%s", JS_IsObject(e) ? "object" : "value") ;
		}
	}
	const char *message = (text != NULL ? text : "") ;
	ugly_status st ;
	if (JS_IsNull(e)||JS_IsUndefined(e)){
		JS_FreeValue(c, e) ;
		st = ugly_raise_native(ctx, r->urt, type, message, NULL, 0) ;
	}
	else {
		uintptr_t identity = (JS_VALUE_HAS_REF_COUNT(e) ? (uintptr_t)JS_VALUE_GET_PTR(e) : 0) ;
		jsbox *box = new_box(r, e) ;     /* the payload owns the exception's reference */
		st = (box == NULL ? ugly_raise_native(ctx, r->urt, type, message, NULL, 0)
			: ugly_raise_native(ctx, r->urt, type, message, box, identity)) ;
	}
	JS_FreeCString(c, text) ;
	return st ;
}


/* ---------------------------------------------------------------- */
/* Proxies: foreign values inside JS                                */
/* ---------------------------------------------------------------- */

static ugly_ctx *need_ctx(JSContext *c){
	ugly_ctx *ctx = ugly_current_ctx() ;
	if (ctx == NULL){
		JS_ThrowInternalError(c, "ugly: no ctx on this thread") ;
		return NULL ;
	}
	run_later() ;     /* JS is calling out: what finalizers had to release goes now */
	return ctx ;
}


/* JS args -> owned ugly_vals. Returns how many were made; on failure, -1 with a JS exception pending. */
static int js_args(JSContext *c, ugly_ctx *ctx, int argc, JSValueConst *argv, ugly_val **out){
	*out = calloc((size_t)argc + 1, sizeof(ugly_val)) ;
	int i ;
	for (i = 0 ; i < argc ; i++){
		ugly_status st = to_val(ctx, R(c), argv[i], UGLY_ANY, &(*out)[i]) ;
		if (st != UGLY_OK){
			int j ;
			for (j = 0 ; j < i ; j++){
				ugly_val_release(ctx, &(*out)[j]) ;
			}
			free(*out) ;
			*out = NULL ;
			throw_ugly(c, ctx, st) ;
			return -1 ;
		}
	}
	return argc ;
}


static void free_args(ugly_ctx *ctx, ugly_val *args, int n){
	int i ;
	for (i = 0 ; i < n ; i++){
		ugly_val_release(ctx, &args[i]) ;
	}
	free(args) ;
}


/* A core result as a JS value (and releases it), or throws. */
static JSValue result(JSContext *c, ugly_ctx *ctx, ugly_status st, ugly_val *out){
	if (st != UGLY_OK){
		return throw_ugly(c, ctx, st) ;
	}
	JSValue v = from_val(ctx, R(c), *out) ;
	ugly_val_release(ctx, out) ;
	return v ;
}


/*
 * Keyword arguments (contract §11): ugly.kw({ name: value }) as the last
 * argument of a call, an object of kw_class holding the properties in their
 * order. Names and values are owned (free_kw).
 */
typedef struct { ugly_opts o ; ugly_kwarg *kw ; } js_kw ;

/* A string property of an ugly.opts object, copied (NULL if absent). */
static char *opts_string(JSContext *c, JSValueConst obj, const char *name){
	JSValue v = JS_GetPropertyStr(c, obj, name) ;
	char *s = NULL ;
	if (JS_IsString(v)){
		const char *t = JS_ToCString(c, v) ;
		s = (t != NULL ? strdup(t) : NULL) ;
		JS_FreeCString(c, t) ;
	}
	JS_FreeValue(c, v) ;
	return s ;
}


/* Takes the trailing ugly.kw({...}) and ugly.opts({...}) (one of each at most, either order) off argv. */
static int take_kw(JSContext *c, ugly_ctx *ctx, int *argc, JSValueConst *argv, js_kw *k){
	k->o = (ugly_opts)UGLY_OPTS_INIT ;
	k->kw = NULL ;
	int kwi = -1 ;
	while (*argc > 0){
		JSValueConst last = argv[*argc - 1] ;
		if ((k->o.hint == NULL)&&(k->o.context == NULL)&&(JS_GetOpaque(last, opts_class) != NULL)){
			k->o.hint = opts_string(c, last, "hint") ;
			k->o.context = opts_string(c, last, "context") ;
		}
		else if ((kwi < 0)&&(JS_GetOpaque(last, kw_class) != NULL)){
			kwi = *argc - 1 ;
		}
		else {
			break ;
		}
		(*argc)-- ;
	}
	if (kwi < 0){
		return 0 ;
	}
	JSValueConst obj = argv[kwi] ;
	JSPropertyEnum *tab = NULL ;
	uint32_t n = 0, i ;
	if (JS_GetOwnPropertyNames(c, &tab, &n, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0){
		return -1 ;
	}
	k->kw = calloc(n + 1, sizeof(ugly_kwarg)) ;
	int bad = 0 ;
	size_t made = 0 ;
	for (i = 0 ; i < n ; i++){
		if (! bad){
			JSValue v = JS_GetProperty(c, obj, tab[i].atom) ;
			const char *name = JS_AtomToCString(c, tab[i].atom) ;
			ugly_status st = ((JS_IsException(v)||(name == NULL)) ? UGLY_ERR_INTERNAL
				: to_val(ctx, R(c), v, UGLY_ANY, &k->kw[made].value)) ;
			JS_FreeValue(c, v) ;
			if (st == UGLY_OK){
				k->kw[made].name = strdup(name) ;
				made++ ;
			}
			else {
				bad = 1 ;
				if (st != UGLY_ERR_INTERNAL){
					throw_ugly(c, ctx, st) ;
				}
			}
			JS_FreeCString(c, name) ;
		}
		JS_FreeAtom(c, tab[i].atom) ;
	}
	js_free(c, tab) ;
	k->o.kwargs = k->kw ;
	k->o.nkwargs = made ;
	return bad ? -1 : 0 ;
}

static void free_kw(ugly_ctx *ctx, js_kw *k){
	free((char *)k->o.hint) ;
	free((char *)k->o.context) ;
	size_t i ;
	for (i = 0 ; i < k->o.nkwargs ; i++){
		free((char *)k->kw[i].name) ;
		ugly_val_release(ctx, &k->kw[i].value) ;
	}
	free(k->kw) ;
}


/* proxy(...) and new proxy(...). */
static JSValue proxy_call(JSContext *c, JSValueConst func, JSValueConst this_val, int argc, JSValueConst *argv,
	int flags){

	(void)this_val ;
	jsproxy *p = proxy_of(func) ;
	ugly_ctx *ctx = need_ctx(c) ;
	if ((p == NULL)||(ctx == NULL)){
		return JS_EXCEPTION ;
	}
	js_kw k ;
	if (take_kw(c, ctx, &argc, argv, &k) < 0){
		free_kw(ctx, &k) ;
		return JS_EXCEPTION ;
	}
	ugly_val *a ;
	int n = js_args(c, ctx, argc, argv, &a) ;
	if (n < 0){
		free_kw(ctx, &k) ;
		return JS_EXCEPTION ;
	}
	ugly_val out = ugly_null() ;
	ugly_status st ;
	if (flags & JS_CALL_FLAG_CONSTRUCTOR){
		ugly_handle h = UGLY_NO_HANDLE ;
		st = ugly_new_opts(ctx, p->h, a, (size_t)n, &k.o, &h) ;
		out = ugly_hval(h) ;
	}
	else {
		st = ugly_call_opts(ctx, p->h, a, (size_t)n, &k.o, UGLY_ANY, &out) ;
	}
	free_args(ctx, a, n) ;
	free_kw(ctx, &k) ;
	return result(c, ctx, st, &out) ;
}


/* A method of a foreign object, bound to it: data = { proxy, name }. */
static JSValue proxy_method(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
	JSValue *data){

	(void)this_val ; (void)magic ;
	jsproxy *p = proxy_of(data[0]) ;
	ugly_ctx *ctx = need_ctx(c) ;
	const char *name = JS_ToCString(c, data[1]) ;
	if ((p == NULL)||(ctx == NULL)||(name == NULL)){
		JS_FreeCString(c, name) ;
		return JS_EXCEPTION ;
	}
	js_kw k ;
	int n = -1 ;
	ugly_val *a = NULL ;
	ugly_val out = ugly_null() ;
	ugly_status st = UGLY_OK ;
	if (take_kw(c, ctx, &argc, argv, &k) == 0){
		n = js_args(c, ctx, argc, argv, &a) ;
	}
	if (n >= 0){
		st = ugly_invoke_opts(ctx, p->h, name, a, (size_t)n, &k.o, UGLY_ANY, &out) ;
		free_args(ctx, a, n) ;
	}
	free_kw(ctx, &k) ;
	JS_FreeCString(c, name) ;
	return (n < 0 ? JS_EXCEPTION : result(c, ctx, st, &out)) ;
}


/* An array index atom ("0", "1", ...) as a number, or -1. */
static int64_t atom_index(JSContext *c, JSAtom atom){
	JSValue v = JS_AtomToValue(c, atom) ;
	int64_t i = -1 ;
	if (JS_IsNumber(v)){
		double d ;
		JS_ToFloat64(c, &d, v) ;
		if ((d >= 0)&&(d == floor(d))&&(d < 4294967295.0)){
			i = (int64_t)d ;
		}
	}
	else if (JS_IsString(v)){
		const char *s = JS_ToCString(c, v) ;
		if ((s != NULL)&&(s[0] >= '0')&&(s[0] <= '9')&&((s[0] != '0')||(s[1] == '\0'))){
			char *end ;
			long long n = strtoll(s, &end, 10) ;
			if (*end == '\0'){
				i = n ;
			}
		}
		JS_FreeCString(c, s) ;
	}
	JS_FreeValue(c, v) ;
	return i ;
}


/* [[GetOwnProperty]]: elements by index, "length" for element collections, members by name. */
static int proxy_get_own_property(JSContext *c, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom atom){
	jsproxy *p = proxy_of(obj) ;
	ugly_ctx *ctx = need_ctx(c) ;
	if ((p == NULL)||(ctx == NULL)){
		return -1 ;
	}
	JSValue key = JS_AtomToValue(c, atom) ;
	int symbol = JS_IsSymbol(key) ;
	JS_FreeValue(c, key) ;
	if (symbol){
		return 0 ;     /* symbols (Symbol.iterator, toPrimitive...) come from the prototype */
	}

	ugly_val out = ugly_null() ;
	ugly_status st ;
	JSValue v = JS_UNDEFINED ;
	int64_t i = atom_index(c, atom) ;
	const char *name = NULL ;
	if ((i >= 0)&&(p->traits & UGLY_TRAIT_ELEMENTS)){
		st = ugly_get_index(ctx, p->h, i, UGLY_ANY, &out) ;
	}
	else {
		name = JS_AtomToCString(c, atom) ;
		if (name == NULL){
			return -1 ;
		}
		if ((strcmp(name, "length") == 0)&&(p->traits & UGLY_TRAIT_ELEMENTS)){
			int64_t n = 0 ;
			st = ugly_length(ctx, p->h, &n) ;
			out = ugly_int(n) ;
		}
		else {
			st = ugly_get_member(ctx, p->h, name, UGLY_ANY, &out) ;
		}
	}
	if ((st == UGLY_ERR_NOT_FOUND)||(st == UGLY_ERR_NOT_CAPABLE)){
		JS_FreeCString(c, name) ;
		return 0 ;     /* not there: JS goes on to the prototype */
	}
	if (st != UGLY_OK){
		JS_FreeCString(c, name) ;
		throw_ugly(c, ctx, st) ;
		return -1 ;
	}

	/* A callable member is a method: bind it to the proxy (receiver rules are the foreign language's). */
	uint32_t traits = 0 ;
	ugly_val tn ;
	if ((name != NULL)&&(out.kind == UGLY_HANDLE)&&(ugly_describe(ctx, out.as.h, &traits, &tn) == UGLY_OK)){
		ugly_val_release(ctx, &tn) ;
	}
	/* A callable member is a method; a class is the class (new obj.Class(...)), as in Lua. */
	if ((name != NULL)&&(traits & UGLY_TRAIT_CALLABLE)&&(! (traits & UGLY_TRAIT_INSTANTIABLE))){
		ugly_val_release(ctx, &out) ;
		JSValue data[2] = { JS_DupValue(c, obj), JS_NewString(c, name) } ;
		v = JS_NewCFunctionData(c, proxy_method, 0, 0, 2, data) ;
		JS_FreeValue(c, data[0]) ;
		JS_FreeValue(c, data[1]) ;
	}
	else {
		v = result(c, ctx, UGLY_OK, &out) ;
	}
	JS_FreeCString(c, name) ;
	if (JS_IsException(v)){
		return -1 ;
	}
	if (desc != NULL){
		desc->flags = JS_PROP_C_W_E ;
		desc->value = v ;
		desc->getter = JS_UNDEFINED ;
		desc->setter = JS_UNDEFINED ;
	}
	else {
		JS_FreeValue(c, v) ;
	}
	return 1 ;
}


/* [[DefineOwnProperty]] (assignment ends up here): set_index or set_member. */
static int proxy_define_own_property(JSContext *c, JSValueConst obj, JSAtom atom, JSValueConst val,
	JSValueConst getter, JSValueConst setter, int flags){

	(void)getter ; (void)setter ; (void)flags ;
	jsproxy *p = proxy_of(obj) ;
	ugly_ctx *ctx = need_ctx(c) ;
	if ((p == NULL)||(ctx == NULL)){
		return -1 ;
	}
	/* A symbol names no foreign member: its description would be stored as a string key. */
	JSValue key = JS_AtomToValue(c, atom) ;
	int symbol = JS_IsSymbol(key) ;
	JS_FreeValue(c, key) ;
	if (symbol){
		JS_ThrowTypeError(c, "ugly: a foreign value has no symbol-keyed properties") ;
		return -1 ;
	}
	ugly_val v ;
	ugly_status st = to_val(ctx, R(c), val, UGLY_ANY, &v) ;
	if (st == UGLY_OK){
		int64_t i = atom_index(c, atom) ;
		if ((i >= 0)&&(p->traits & UGLY_TRAIT_ELEMENTS)){
			st = ugly_set_index(ctx, p->h, i, v) ;
		}
		else {
			const char *name = JS_AtomToCString(c, atom) ;
			st = (name != NULL ? ugly_set_member(ctx, p->h, name, v) : UGLY_ERR_USAGE) ;
			JS_FreeCString(c, name) ;
		}
		ugly_val_release(ctx, &v) ;
	}
	if (st != UGLY_OK){
		throw_ugly(c, ctx, st) ;
		return -1 ;
	}
	return 1 ;
}


static int proxy_get_own_property_names(JSContext *c, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj){
	jsproxy *p = proxy_of(obj) ;
	ugly_ctx *ctx = need_ctx(c) ;
	*ptab = NULL ;
	*plen = 0 ;
	if ((p == NULL)||(ctx == NULL)){
		return -1 ;
	}
	ugly_handle names ;
	if (ugly_member_names(ctx, p->h, &names) != UGLY_OK){
		return 0 ;     /* no members */
	}
	int64_t n = 0 ;
	ugly_length(ctx, names, &n) ;
	JSPropertyEnum *tab = js_malloc(c, sizeof(JSPropertyEnum) * (size_t)(n > 0 ? n : 1)) ;
	if (tab == NULL){
		ugly_release(ctx, names) ;
		return -1 ;
	}
	uint32_t k = 0 ;
	int64_t i ;
	for (i = 0 ; i < n ; i++){
		ugly_val s ;
		if (ugly_get_index(ctx, names, i, UGLY_STRING, &s) == UGLY_OK){
			tab[k].is_enumerable = 1 ;
			tab[k].atom = JS_NewAtomLen(c, s.as.str.ptr, s.as.str.len) ;
			k++ ;
			ugly_val_release(ctx, &s) ;
		}
	}
	ugly_release(ctx, names) ;
	*ptab = tab ;
	*plen = k ;
	return 0 ;
}


static int proxy_delete_property(JSContext *c, JSValueConst obj, JSAtom atom){
	(void)c ; (void)obj ; (void)atom ;
	return 0 ;     /* foreign members can't be deleted */
}


static void proxy_finalizer(JSRuntime *rt, JSValue val){
	(void)rt ;
	jsproxy *p = proxy_of(val) ;
	if (p != NULL){
		if (registered(p->r)){
			pc_remove(p->r, p->h, JS_VALUE_GET_PTR(val)) ;
		}
		release_later(p->h, NULL) ;
		free(p) ;
	}
}


static void error_finalizer(JSRuntime *rt, JSValue val){
	(void)rt ;
	errbox *b = JS_GetOpaque(val, error_class) ;
	if (b != NULL){
		release_later(UGLY_NO_HANDLE, b->err) ;
		free(b) ;
	}
}


/* A step of a foreign iterator, as JS's iterator protocol: { value, done }. data[0] is the iterator's proxy. */
static JSValue iter_step(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data){
	(void)this_val ; (void)argc ; (void)argv ; (void)magic ;
	jsproxy *p = proxy_of(data[0]) ;
	ugly_ctx *ctx = need_ctx(c) ;
	if ((p == NULL)||(ctx == NULL)){
		return JS_EXCEPTION ;
	}
	ugly_val v = ugly_null() ;
	size_t got = 0 ;
	ugly_status st = ugly_next(ctx, p->h, UGLY_ANY, &v, 1, &got) ;
	if (st != UGLY_OK){
		return throw_ugly(c, ctx, st) ;
	}
	JSValue value = (got == 1 ? result(c, ctx, UGLY_OK, &v) : JS_UNDEFINED) ;
	if (JS_IsException(value)){
		return value ;
	}
	JSValue res = JS_NewObject(c) ;
	JS_SetPropertyStr(c, res, "value", value) ;
	JS_SetPropertyStr(c, res, "done", JS_NewBool(c, got == 0)) ;
	return res ;
}


static JSValue iter_self(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv){
	(void)argc ; (void)argv ;
	return JS_DupValue(c, this_val) ;
}


/*
 * proxy[Symbol.iterator](), for for...of, spread, Array.from: the foreign
 * value's own iteration, one value per step, as a JS iterator over its
 * iterator's proxy (a foreign iterator's own "next" member is its
 * language's, not JS's protocol). A value its language doesn't iterate but
 * with elements gets what JS gives any array-like: Array.prototype.values.
 */
static JSValue proxy_iterate(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv){
	(void)argc ; (void)argv ;
	jsproxy *p = proxy_of(this_val) ;
	ugly_ctx *ctx = need_ctx(c) ;
	if ((p == NULL)||(ctx == NULL)){
		return (ctx == NULL ? JS_EXCEPTION : JS_ThrowTypeError(c, "ugly: not a proxy")) ;
	}
	ugly_handle it = UGLY_NO_HANDLE ;
	ugly_status st = ugly_iter(ctx, p->h, &it) ;
	if (((st == UGLY_ERR_NOT_CAPABLE)||(st == UGLY_ERR_NOT_IMPLEMENTED))&&(p->traits & UGLY_TRAIT_ELEMENTS)){
		JSValue global = JS_GetGlobalObject(c) ;
		JSValue array = JS_GetPropertyStr(c, global, "Array") ;
		JSValue proto = JS_GetPropertyStr(c, array, "prototype") ;
		JSValue values = JS_GetPropertyStr(c, proto, "values") ;
		JSValue r = JS_Call(c, values, this_val, 0, NULL) ;
		JS_FreeValue(c, values) ;
		JS_FreeValue(c, proto) ;
		JS_FreeValue(c, array) ;
		JS_FreeValue(c, global) ;
		return r ;
	}
	if ((st == UGLY_ERR_NOT_CAPABLE)||(st == UGLY_ERR_NOT_IMPLEMENTED)){
		return JS_ThrowTypeError(c, "ugly: the foreign value is not iterable") ;
	}
	ugly_val v = ugly_hval(it) ;
	JSValue itp = result(c, ctx, st, &v) ;
	if (JS_IsException(itp)){
		return itp ;
	}
	if (proxy_of(itp) == NULL){
		return itp ;     /* its iterator is a JS value, come home: JS's own */
	}
	JSValue obj = JS_NewObject(c) ;
	JS_SetPropertyStr(c, obj, "next", JS_NewCFunctionData(c, iter_step, 0, 0, 1, &itp)) ;
	JS_SetProperty(c, obj, R(c)->iterator, JS_NewCFunction(c, iter_self, "[Symbol.iterator]", 0)) ;
	JS_FreeValue(c, itp) ;
	return obj ;
}


/* proxy.toString(), and so String(proxy) and `${proxy}`: its language's (ugly_to_string). */
static JSValue proxy_to_string(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv){
	(void)argc ; (void)argv ;
	jsproxy *p = proxy_of(this_val) ;
	ugly_ctx *ctx = need_ctx(c) ;
	if ((p == NULL)||(ctx == NULL)){
		return (ctx == NULL ? JS_EXCEPTION : JS_ThrowTypeError(c, "ugly: not a proxy")) ;
	}
	ugly_val v = ugly_null() ;
	ugly_status st = ugly_to_string(ctx, p->h, &v) ;
	if ((st == UGLY_ERR_NOT_SUPPORTED)||(st == UGLY_ERR_NOT_IMPLEMENTED)){
		return JS_NewString(c, "[object ugly.Proxy]") ;
	}
	if (st != UGLY_OK){
		return throw_ugly(c, ctx, st) ;
	}
	JSValue s = (v.kind == UGLY_STRING ? JS_NewStringLen(c, v.as.str.ptr, v.as.str.len)
		: JS_NewStringLen(c, (const char *)v.as.bytes.ptr, v.as.bytes.len)) ;
	ugly_val_release(ctx, &v) ;
	return s ;
}


static JSClassExoticMethods proxy_exotic = {
	.get_own_property = proxy_get_own_property,
	.get_own_property_names = proxy_get_own_property_names,
	.delete_property = proxy_delete_property,
	.define_own_property = proxy_define_own_property,
} ;

static JSClassDef proxy_def = { "ugly.Proxy", .finalizer = proxy_finalizer, .exotic = &proxy_exotic } ;
static JSClassDef proxy_fn_def = { "ugly.Proxy", .finalizer = proxy_finalizer, .call = proxy_call, .exotic = &proxy_exotic } ;
static JSClassDef error_def = { "ugly.Error", .finalizer = error_finalizer } ;
static JSClassDef kw_def = { "ugly.Kw" } ;
static JSClassDef opts_def = { "ugly.Opts" } ;


/* ---------------------------------------------------------------- */
/* The "ugly" module                                                */
/* ---------------------------------------------------------------- */

static ugly_rt *other(JSContext *c, ugly_ctx *ctx, JSValueConst lang_val, const char **lang){
	*lang = JS_ToCString(c, lang_val) ;
	if (*lang == NULL){
		return NULL ;
	}
	if (strcmp(*lang, "js") == 0){
		JS_ThrowTypeError(c, "ugly: use import and eval for JS itself") ;
		return NULL ;
	}
	ugly_rt *rt ;
	ugly_status st = ugly_rt_get(ctx, *lang, &rt) ;
	if (st != UGLY_OK){
		throw_ugly(c, ctx, st) ;
		return NULL ;
	}
	return rt ;
}


/* ugly.module(lang, name): a module of another language. */
static JSValue api_module(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv){
	(void)this_val ;
	ugly_ctx *ctx = need_ctx(c) ;
	if ((ctx == NULL)||(argc < 2)){
		return (ctx == NULL ? JS_EXCEPTION : JS_ThrowTypeError(c, "ugly.module(lang, name)")) ;
	}
	const char *lang = NULL ;
	ugly_rt *rt = other(c, ctx, argv[0], &lang) ;
	const char *name = (rt != NULL ? JS_ToCString(c, argv[1]) : NULL) ;
	JSValue ret = JS_EXCEPTION ;
	if (name != NULL){
		ugly_handle h ;
		ugly_status st = ugly_rt_import(ctx, rt, name, &h) ;
		ugly_val v = ugly_hval(h) ;
		ret = result(c, ctx, st, &v) ;
	}
	JS_FreeCString(c, lang) ;
	JS_FreeCString(c, name) ;
	return ret ;
}


/* ugly.eval(lang, code): the value of code in another language. */
static JSValue api_eval(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv){
	(void)this_val ;
	ugly_ctx *ctx = need_ctx(c) ;
	if ((ctx == NULL)||(argc < 2)){
		return (ctx == NULL ? JS_EXCEPTION : JS_ThrowTypeError(c, "ugly.eval(lang, code)")) ;
	}
	const char *lang = NULL ;
	ugly_rt *rt = other(c, ctx, argv[0], &lang) ;
	size_t len ;
	const char *code = (rt != NULL ? JS_ToCStringLen(c, &len, argv[1]) : NULL) ;
	JSValue ret = JS_EXCEPTION ;
	if (code != NULL){
		ugly_val out = ugly_null() ;
		ugly_status st = ugly_rt_eval(ctx, rt, code, len, UGLY_ANY, &out) ;
		ret = result(c, ctx, st, &out) ;
	}
	JS_FreeCString(c, lang) ;
	JS_FreeCString(c, code) ;
	return ret ;
}


/* ugly_configure for one value; 0, or -1 with a JS exception thrown. */
static int configure_value(JSContext *c, ugly_ctx *ctx, const char *lang, const char *key, JSValueConst v){
	if (! JS_IsString(v)){
		JS_ThrowTypeError(c, "ugly.configure: %s takes a string or an array of strings", key) ;
		return -1 ;
	}
	const char *s = JS_ToCString(c, v) ;
	if (s == NULL){
		return -1 ;
	}
	ugly_status st = ugly_configure(ctx, lang, key, s) ;
	JS_FreeCString(c, s) ;
	if (st != UGLY_OK){
		throw_ugly(c, ctx, st) ;
		return -1 ;
	}
	return 0 ;
}


/* ugly.configure(lang, { path: "dir" or [...], ... }): options for lang, before its first use. */
static JSValue api_configure(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv){
	(void)this_val ;
	ugly_ctx *ctx = need_ctx(c) ;
	if ((ctx == NULL)||(argc < 2)||(! JS_IsObject(argv[1]))){
		return (ctx == NULL ? JS_EXCEPTION : JS_ThrowTypeError(c, "ugly.configure(lang, { option: value, ... })")) ;
	}
	const char *lang = JS_ToCString(c, argv[0]) ;
	if (lang == NULL){
		return JS_EXCEPTION ;
	}
	JSPropertyEnum *tab = NULL ;
	uint32_t n = 0, i ;
	int bad = (JS_GetOwnPropertyNames(c, &tab, &n, argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) ;
	for (i = 0 ; (! bad)&&(i < n) ; i++){
		const char *key = JS_AtomToCString(c, tab[i].atom) ;
		JSValue v = JS_GetProperty(c, argv[1], tab[i].atom) ;
		if ((key == NULL)||JS_IsException(v)){
			bad = 1 ;
		}
		else if (JS_IsArray(c, v)){
			int64_t len = 0, k ;
			JSValue jl = JS_GetPropertyStr(c, v, "length") ;
			bad = (JS_ToInt64(c, &len, jl) < 0) ;
			JS_FreeValue(c, jl) ;
			for (k = 0 ; (! bad)&&(k < len) ; k++){
				JSValue item = JS_GetPropertyUint32(c, v, (uint32_t)k) ;
				bad = (JS_IsException(item) || (configure_value(c, ctx, lang, key, item) != 0)) ;
				JS_FreeValue(c, item) ;
			}
		}
		else {
			bad = (configure_value(c, ctx, lang, key, v) != 0) ;
		}
		JS_FreeValue(c, v) ;
		JS_FreeCString(c, key) ;
	}
	for (i = 0 ; i < n ; i++){
		JS_FreeAtom(c, tab[i].atom) ;
	}
	js_free(c, tab) ;
	JS_FreeCString(c, lang) ;
	return (bad ? JS_EXCEPTION : JS_UNDEFINED) ;
}


/* A marker object of class cls (ugly.kw, ugly.opts) with argv[0]'s own properties, in order. */
static JSValue marker(JSContext *c, JSClassID cls, int argc, JSValueConst *argv, const char *usage){
	if ((argc < 1)||(! JS_IsObject(argv[0]))){
		return JS_ThrowTypeError(c, "%s", usage) ;
	}
	JSValue k = JS_NewObjectClass(c, (int)cls) ;
	if (JS_IsException(k)){
		return k ;
	}
	JS_SetOpaque(k, (void *)1) ;     /* JS_GetOpaque(v, kw_class) finds it */
	JSPropertyEnum *tab = NULL ;
	uint32_t n = 0, i ;
	if (JS_GetOwnPropertyNames(c, &tab, &n, argv[0], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0){
		JS_FreeValue(c, k) ;
		return JS_EXCEPTION ;
	}
	for (i = 0 ; i < n ; i++){
		JS_SetProperty(c, k, tab[i].atom, JS_GetProperty(c, argv[0], tab[i].atom)) ;
		JS_FreeAtom(c, tab[i].atom) ;
	}
	js_free(c, tab) ;
	return k ;
}


/* ugly.kw({ name: value, ... }): keyword arguments, for the last argument of a call. */
static JSValue api_kw(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv){
	(void)this_val ;
	return marker(c, kw_class, argc, argv, "ugly.kw({ name: value, ... })") ;
}


/* ugly.opts({ hint: "...", context: "scalar" }): call options, for the last argument of a call. */
static JSValue api_opts(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv){
	(void)this_val ;
	return marker(c, opts_class, argc, argv, "ugly.opts({ hint: ..., context: ... })") ;
}


static JSValue api_is_error(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv){
	(void)this_val ;
	return JS_NewBool(c, (argc > 0)&&(JS_GetOpaque(argv[0], error_class) != NULL)) ;
}


static JSValue api_is_proxy(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv){
	(void)this_val ;
	return JS_NewBool(c, (argc > 0)&&(proxy_of(argv[0]) != NULL)) ;
}


/* ugly.gc(): collect now, and run the finalization callbacks this queued (QuickJS queues them only at GC). */
static JSValue api_gc(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv){
	(void)this_val ; (void)argc ; (void)argv ;
	jsrt *r = R(c) ;
	JS_RunGC(r->rt) ;
	run_jobs(r) ;
	return JS_UNDEFINED ;
}


static const JSCFunctionListEntry api[] = {
	JS_CFUNC_DEF("module", 2, api_module),
	JS_CFUNC_DEF("eval", 2, api_eval),
	JS_CFUNC_DEF("configure", 2, api_configure),
	JS_CFUNC_DEF("kw", 1, api_kw),
	JS_CFUNC_DEF("opts", 1, api_opts),
	JS_CFUNC_DEF("isError", 1, api_is_error),
	JS_CFUNC_DEF("isProxy", 1, api_is_proxy),
	JS_CFUNC_DEF("gc", 0, api_gc),
} ;


static int api_init(JSContext *c, JSModuleDef *m){
	return JS_SetModuleExportList(c, m, api, sizeof(api) / sizeof(api[0])) ;
}


/* The module search path: ugly_configure("js", "path", ...) in the order given, then UGLY_JS_PATH. */
static char *module_path(void){
	const char *env = getenv("UGLY_JS_PATH"), *v ;
	size_t len = (env != NULL ? strlen(env) : 0) + 1, i ;
	for (i = 0 ; (v = ugly_option("js", "path", i)) != NULL ; i++){
		len += strlen(v) + 1 ;
	}
	char *search = malloc(len) ;
	if (search == NULL){
		return NULL ;
	}
	search[0] = '\0' ;
	for (i = 0 ; (v = ugly_option("js", "path", i)) != NULL ; i++){
		strcat(search, v) ;
		strcat(search, ":") ;
	}
	strcat(search, env != NULL ? env : "") ;
	return search ;
}


/* Loads module 'name': "ugly" is built in; others are <dir>/<name>.js for a dir in the module path. */
static JSModuleDef *module_loader(JSContext *c, const char *name, void *opaque){
	(void)opaque ;
	if (strcmp(name, "ugly") == 0){
		JSModuleDef *m = JS_NewCModule(c, "ugly", api_init) ;
		if (m != NULL){
			JS_AddModuleExportList(c, m, api, sizeof(api) / sizeof(api[0])) ;
		}
		return m ;
	}

	char *search = module_path() ;
	char *save = NULL, *dir ;
	char *src = NULL ;
	size_t len = 0 ;
	for (dir = (search != NULL ? strtok_r(search, ":", &save) : NULL) ; (dir != NULL)&&(src == NULL) ; dir = strtok_r(NULL, ":", &save)){
		char path[4096] ;
		size_t nl = strlen(name) ;
		snprintf(path, sizeof(path), "%s/%s%s", dir, name, ((nl > 3)&&(strcmp(name + nl - 3, ".js") == 0)) ? "" : ".js") ;
		FILE *f = fopen(path, "rb") ;
		if (f == NULL){
			continue ;
		}
		fseek(f, 0, SEEK_END) ;
		long size = ftell(f) ;
		fseek(f, 0, SEEK_SET) ;
		src = (size >= 0 ? malloc((size_t)size + 1) : NULL) ;
		if ((src != NULL)&&(fread(src, 1, (size_t)size, f) == (size_t)size)){
			src[size] = '\0' ;     /* JS_Eval needs a terminated buffer */
			len = (size_t)size ;
		}
		else {
			free(src) ;
			src = NULL ;
		}
		fclose(f) ;
	}
	free(search) ;
	if (src == NULL){
		JS_ThrowReferenceError(c, "could not load module '%s' (not in the \"path\" option or UGLY_JS_PATH)", name) ;
		return NULL ;
	}
	JSValue fn = JS_Eval(c, src, len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY) ;
	free(src) ;
	if (JS_IsException(fn)){
		return NULL ;
	}
	JSModuleDef *m = JS_VALUE_GET_PTR(fn) ;
	JS_FreeValue(c, fn) ;
	return m ;
}


/* ---------------------------------------------------------------- */
/* Operations                                                       */
/* ---------------------------------------------------------------- */

static jsrt *RT(ugly_rt *rt){
	return ugly_rt_native(rt) ;
}


/* Finishes an operation that produced v (consumed), or a pending exception. */
static ugly_status out_val(ugly_ctx *ctx, jsrt *r, JSValue v, ugly_kind want, ugly_val *out){
	if (JS_IsException(v)){
		return js_failure(ctx, r) ;
	}
	ugly_status st = to_val(ctx, r, v, want, out) ;
	JS_FreeValue(r->c, v) ;
	return st ;
}


/* Converts args to JS values (new references). NULL with st set on failure. */
static JSValue *js_values(ugly_ctx *ctx, jsrt *r, const ugly_val *args, size_t n, ugly_status *st){
	JSValue *a = calloc(n + 1, sizeof(JSValue)) ;
	size_t i ;
	*st = UGLY_OK ;
	for (i = 0 ; i < n ; i++){
		a[i] = from_val(ctx, r, args[i]) ;
		if (JS_IsException(a[i])){
			size_t j ;
			for (j = 0 ; j < i ; j++){
				JS_FreeValue(r->c, a[j]) ;
			}
			free(a) ;
			*st = js_failure(ctx, r) ;
			return NULL ;
		}
	}
	return a ;
}


static void free_values(jsrt *r, JSValue *a, size_t n){
	size_t i ;
	for (i = 0 ; i < n ; i++){
		JS_FreeValue(r->c, a[i]) ;
	}
	free(a) ;
}


static ugly_status js_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out){
	jsrt *r = RT(rt) ;
	JSContext *c = r->c ;
	enter(r) ;
	/* Loading evaluates the module: a promise of its namespace, settled once the jobs run. */
	JSValue promise = JS_LoadModule(c, "", name) ;
	if (JS_IsException(promise)){
		return leave(r, js_failure(ctx, r)) ;
	}
	run_jobs(r) ;
	ugly_status st ;
	switch (JS_PromiseState(c, promise)){
		case JS_PROMISE_FULFILLED:
			st = out_val(ctx, r, JS_PromiseResult(c, promise), UGLY_HANDLE, out) ;
			break ;
		case JS_PROMISE_REJECTED:
			JS_Throw(c, JS_PromiseResult(c, promise)) ;
			st = js_failure(ctx, r) ;
			break ;
		default:
			st = fail(ctx, r, UGLY_ERR_INTERNAL, "Error", "module evaluation didn't settle") ;
	}
	JS_FreeValue(c, promise) ;
	return leave(r, st) ;
}


static ugly_status js_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_val *out){
	jsrt *r = RT(rt) ;
	enter(r) ;
	return leave(r, out_val(ctx, r, JS_GetGlobalObject(r->c), UGLY_HANDLE, out)) ;
}


static ugly_status js_eval(ugly_ctx *ctx, ugly_rt *rt, const char *code, size_t len, ugly_kind want, ugly_val *out){
	jsrt *r = RT(rt) ;
	char *src = malloc(len + 1) ;     /* JS_Eval needs a terminated buffer */
	if (src == NULL){
		return UGLY_ERR_NOMEM ;
	}
	memcpy(src, code, len) ;
	src[len] = '\0' ;
	enter(r) ;
	ugly_status st = out_val(ctx, r, JS_Eval(r->c, src, len, "<eval>", JS_EVAL_TYPE_GLOBAL), want, out) ;
	free(src) ;
	return leave(r, st) ;
}


/*
 * Whether a constructor is a class: one that can't be called, only
 * constructed. JS keeps no such flag where we can read it, but a class's
 * source text begins with "class" (Function.prototype.toString, not the
 * value's own toString, which is the program's to define). A function
 * compiled without its source isn't recognized, and stays callable. Asked
 * once per box.
 */
static int is_class(jsrt *r, jsbox *b){
	if (b->is_class < 0){
		JSContext *c = r->c ;
		b->is_class = 0 ;
		JSValue global = JS_GetGlobalObject(c) ;
		JSValue function = JS_GetPropertyStr(c, global, "Function") ;
		JSValue proto = JS_GetPropertyStr(c, function, "prototype") ;
		JSValue to_string = JS_GetPropertyStr(c, proto, "toString") ;
		JSValue text = (JS_IsFunction(c, to_string) ? JS_Call(c, to_string, b->v, 0, NULL) : JS_UNDEFINED) ;
		if (JS_IsException(text)||JS_IsException(to_string)||JS_IsException(proto)||JS_IsException(function)){
			drop_exception(c) ;
		}
		else if (JS_IsString(text)){
			const char *s = JS_ToCString(c, text) ;
			b->is_class = ((s != NULL)&&(strncmp(s, "class", 5) == 0)&&((s[5] == ' ')||(s[5] == '{')||(s[5] == '\n')||(s[5] == '\t'))) ;
			JS_FreeCString(c, s) ;
		}
		JS_FreeValue(c, text) ;
		JS_FreeValue(c, to_string) ;
		JS_FreeValue(c, proto) ;
		JS_FreeValue(c, function) ;
		JS_FreeValue(c, global) ;
	}
	return b->is_class ;
}


static ugly_status js_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name){
	jsrt *r = RT(rt) ;
	JSContext *c = r->c ;
	JSValueConst v = ((jsbox *)ref)->v ;
	enter(r) ;
	uint32_t t = 0 ;
	if (JS_IsObject(v)){
		t |= UGLY_TRAIT_MEMBERS ;
		if (JS_IsArray(c, v) > 0){
			t |= UGLY_TRAIT_ELEMENTS ;
		}
		if (JS_IsFunction(c, v)){
			t |= UGLY_TRAIT_CALLABLE ;
		}
		if (JS_IsConstructor(c, v)){
			/*
			 * A class is INSTANTIABLE and not CALLABLE, as JS has it (calling
			 * one throws), so that calling it from a language with no 'new'
			 * constructs it. A function is both.
			 */
			t |= UGLY_TRAIT_INSTANTIABLE ;
			if (is_class(r, (jsbox *)ref)){
				t &= ~(uint32_t)UGLY_TRAIT_CALLABLE ;
			}
		}
		/* What for...of takes: a Symbol.iterator method (arrays, Map, Set, generators, iterators). */
		JSValue f = JS_GetProperty(c, v, r->iterator) ;
		if (JS_IsFunction(c, f)){
			t |= UGLY_TRAIT_ITERABLE ;
		}
		JS_FreeValue(c, f) ;
		if (JS_HasException(c)){
			drop_exception(c) ;
		}
	}
	*traits = t ;

	/* A module namespace is "module"; other objects are their constructor's name. */
	char name[96] = "" ;
	if (JS_IsObject(v)){
		JSValue tag = JS_GetProperty(c, v, r->to_string_tag) ;
		if (JS_IsString(tag)){
			const char *s = JS_ToCString(c, tag) ;
			if ((s != NULL)&&(strcmp(s, "Module") == 0)){
				snprintf(name, sizeof(name), "module") ;
			}
			JS_FreeCString(c, s) ;
		}
		JS_FreeValue(c, tag) ;
		if (name[0] == '\0'){
			JSValue ctor = JS_GetPropertyStr(c, v, "constructor") ;
			if (JS_IsObject(ctor)){
				str_prop(c, ctor, "name", name, sizeof(name)) ;
			}
			JS_FreeValue(c, ctor) ;
		}
		if (JS_HasException(c)){
			drop_exception(c) ;
		}
	}
	if (name[0] == '\0'){
		snprintf(name, sizeof(name), "%s", JS_IsSymbol(v) ? "symbol" : (JS_IsObject(v) ? "object" : "value")) ;
	}
	return leave(r, ugly_val_set_string(ctx, type_name, name, strlen(name))) ;
}


/* Looks up member 'name' (own or inherited). 0: absent, 1: present (*val set), -1: exception. */
static int member(jsrt *r, JSValueConst obj, const char *name, JSValue *val){
	JSContext *c = r->c ;
	JSAtom atom = JS_NewAtom(c, name) ;
	int has = JS_HasProperty(c, obj, atom) ;
	if (has > 0){
		*val = JS_GetProperty(c, obj, atom) ;
		if (JS_IsException(*val)){
			has = -1 ;
		}
	}
	JS_FreeAtom(c, atom) ;
	return has ;
}


static ugly_status js_get_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want, ugly_val *out){
	jsrt *r = RT(rt) ;
	JSValueConst obj = ((jsbox *)ref)->v ;
	if (! JS_IsObject(obj)){
		return fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no members") ;
	}
	enter(r) ;
	JSValue v ;
	int has = member(r, obj, name, &v) ;
	ugly_status st = (has < 0 ? js_failure(ctx, r)
		: has == 0 ? fail(ctx, r, UGLY_ERR_NOT_FOUND, "ReferenceError", name)
		: out_val(ctx, r, v, want, out)) ;
	return leave(r, st) ;
}


static ugly_status js_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	jsrt *r = RT(rt) ;
	JSValueConst obj = ((jsbox *)ref)->v ;
	if (! JS_IsObject(obj)){
		return fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no members") ;
	}
	enter(r) ;
	JSValue jv = from_val(ctx, r, v) ;
	ugly_status st = UGLY_OK ;
	if (JS_IsException(jv)||(JS_SetPropertyStr(r->c, obj, name, jv) < 0)){
		st = js_failure(ctx, r) ;
	}
	return leave(r, st) ;
}


/* delete obj[name]: true for a missing property, an error for a non-configurable one. */
static ugly_status js_delete_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name){
	jsrt *r = RT(rt) ;
	JSValueConst obj = ((jsbox *)ref)->v ;
	if (! JS_IsObject(obj)){
		return fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no members") ;
	}
	enter(r) ;
	JSAtom a = JS_NewAtom(r->c, name) ;
	ugly_status st = UGLY_OK ;
	if ((a == JS_ATOM_NULL)||(JS_DeleteProperty(r->c, obj, a, JS_PROP_THROW) < 0)){
		st = js_failure(ctx, r) ;
	}
	JS_FreeAtom(r->c, a) ;
	return leave(r, st) ;
}


static ugly_status js_member_names(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	jsrt *r = RT(rt) ;
	JSContext *c = r->c ;
	JSValueConst obj = ((jsbox *)ref)->v ;
	if (! JS_IsObject(obj)){
		return fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no members") ;
	}
	enter(r) ;
	JSPropertyEnum *tab = NULL ;
	uint32_t n = 0, i ;
	if (JS_GetOwnPropertyNames(c, &tab, &n, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0){
		return leave(r, js_failure(ctx, r)) ;
	}
	ugly_val *names = calloc(n + 1, sizeof(ugly_val)) ;
	const char **strs = calloc(n + 1, sizeof(char *)) ;
	for (i = 0 ; i < n ; i++){
		size_t len = 0 ;
		strs[i] = JS_AtomToCStringLen(c, &len, tab[i].atom) ;
		names[i] = ugly_str(strs[i] != NULL ? strs[i] : "", strs[i] != NULL ? len : 0) ;
	}
	ugly_status st = ugly_list_new(ctx, names, n, out) ;
	for (i = 0 ; i < n ; i++){
		JS_FreeCString(c, strs[i]) ;
	}
	free(strs) ;
	free(names) ;
	JS_FreePropertyEnum(c, tab, n) ;
	return leave(r, st) ;
}


/* An array's length, or -1 (not an array) / -2 (exception). */
static int64_t array_length(jsrt *r, JSValueConst obj){
	JSContext *c = r->c ;
	if (JS_IsArray(c, obj) <= 0){
		return -1 ;
	}
	JSValue lv = JS_GetPropertyStr(c, obj, "length") ;
	int64_t n = -2 ;
	if (! JS_IsException(lv)){
		if (JS_ToInt64Ext(c, &n, lv) < 0){
			n = -2 ;
		}
		JS_FreeValue(c, lv) ;
	}
	return n ;
}


static ugly_status js_length(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out){
	jsrt *r = RT(rt) ;
	enter(r) ;
	int64_t n = array_length(r, ((jsbox *)ref)->v) ;
	ugly_status st = (n == -2 ? js_failure(ctx, r)
		: n == -1 ? fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements")
		: UGLY_OK) ;
	if (st == UGLY_OK){
		*out = n ;
	}
	return leave(r, st) ;
}


static ugly_status js_get_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want, ugly_val *out){
	jsrt *r = RT(rt) ;
	JSValueConst obj = ((jsbox *)ref)->v ;
	enter(r) ;
	int64_t n = array_length(r, obj) ;
	ugly_status st ;
	if (n == -2){
		st = js_failure(ctx, r) ;
	}
	else if (n == -1){
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements") ;
	}
	else if ((i < 0)||(i >= n)){
		st = fail(ctx, r, UGLY_ERR_NOT_FOUND, "RangeError", "index out of range") ;
	}
	else {
		st = out_val(ctx, r, JS_GetPropertyUint32(r->c, obj, (uint32_t)i), want, out) ;
	}
	return leave(r, st) ;
}


static ugly_status js_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	jsrt *r = RT(rt) ;
	JSValueConst obj = ((jsbox *)ref)->v ;
	enter(r) ;
	int64_t n = array_length(r, obj) ;
	ugly_status st = UGLY_OK ;
	if (n == -2){
		st = js_failure(ctx, r) ;
	}
	else if (n == -1){
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements") ;
	}
	else if ((i < 0)||(i > n)){
		st = fail(ctx, r, UGLY_ERR_NOT_FOUND, "RangeError", "index out of range") ;
	}
	else {
		JSValue jv = from_val(ctx, r, v) ;     /* one past the end appends */
		if (JS_IsException(jv)||(JS_SetPropertyInt64(r->c, obj, i, jv) < 0)){
			st = js_failure(ctx, r) ;
		}
	}
	return leave(r, st) ;
}


/* Array.prototype.splice.call(obj, offset, remove, ...insert). */
static ugly_status js_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n){

	jsrt *r = RT(rt) ;
	JSContext *c = r->c ;
	JSValueConst obj = ((jsbox *)ref)->v ;
	enter(r) ;
	int64_t len = array_length(r, obj) ;
	if (len == -2){
		return leave(r, js_failure(ctx, r)) ;
	}
	if (len == -1){
		return leave(r, fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements")) ;
	}
	JSValue *argv = calloc(n + 2, sizeof(JSValue)) ;
	if (argv == NULL){
		return leave(r, fail(ctx, r, UGLY_ERR_NOMEM, "Error", "out of memory")) ;
	}
	argv[0] = JS_NewInt64(c, offset) ;
	argv[1] = JS_NewInt64(c, remove) ;
	size_t i, made = 2 ;
	ugly_status st = UGLY_OK ;
	for (i = 0 ; i < n ; i++){
		argv[made] = from_val(ctx, r, insert[i]) ;
		if (JS_IsException(argv[made])){
			st = js_failure(ctx, r) ;
			break ;
		}
		made++ ;
	}
	if (st == UGLY_OK){
		JSValue global = JS_GetGlobalObject(c) ;
		JSValue array = JS_GetPropertyStr(c, global, "Array") ;
		JSValue proto = JS_GetPropertyStr(c, array, "prototype") ;
		JSValue splice = JS_GetPropertyStr(c, proto, "splice") ;
		JSValue res = JS_Call(c, splice, obj, (int)made, argv) ;
		if (JS_IsException(res)){
			st = js_failure(ctx, r) ;
		}
		JS_FreeValue(c, res) ;
		JS_FreeValue(c, splice) ;
		JS_FreeValue(c, proto) ;
		JS_FreeValue(c, array) ;
		JS_FreeValue(c, global) ;
	}
	for (i = 0 ; i < made ; i++){
		JS_FreeValue(c, argv[i]) ;
	}
	free(argv) ;
	return leave(r, st) ;
}


/*
 * Calls f. A native constructor that JS won't call (Map, Set, Promise:
 * "constructor requires 'new'") is constructed instead: a caller in a
 * language with no 'new' can only call. Unlike a class written in JS,
 * describe can't tell such a constructor from one that may be called
 * (Date, String), so the engine's own refusal decides, which it makes
 * before the constructor does anything.
 */
static JSValue call_or_construct(JSContext *c, JSValueConst f, JSValueConst this_obj, int n, JSValue *a){
	JSValue v = JS_Call(c, f, this_obj, n, a) ;
	if ((! JS_IsException(v))||(! JS_IsConstructor(c, f))){
		return v ;
	}
	JSValue e = JS_GetException(c) ;
	int needs_new = 0 ;
	if (JS_IsError(c, e)){
		char name[32], message[64] ;
		str_prop(c, e, "name", name, sizeof(name)) ;
		str_prop(c, e, "message", message, sizeof(message)) ;
		needs_new = (strcmp(name, "TypeError") == 0)
			&&((strcmp(message, "constructor requires 'new'") == 0)||(strcmp(message, "must be called with new") == 0)) ;
	}
	if (! needs_new){
		return JS_Throw(c, e) ;
	}
	JS_FreeValue(c, e) ;
	return JS_CallConstructor(c, f, n, a) ;
}


static ugly_status js_call(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out){

	jsrt *r = RT(rt) ;
	JSValueConst f = ((jsbox *)ref)->v ;
	if (! JS_IsFunction(r->c, f)){
		return fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not callable") ;
	}
	enter(r) ;
	ugly_status st ;
	JSValue *a = js_values(ctx, r, args, nargs, &st) ;
	if (a != NULL){
		st = out_val(ctx, r, call_or_construct(r->c, f, JS_UNDEFINED, (int)nargs, a), want, out) ;
		free_values(r, a, nargs) ;
	}
	return leave(r, st) ;
}


/* obj.method(args), with obj as 'this'. */
static ugly_status js_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out){

	jsrt *r = RT(rt) ;
	JSValueConst obj = ((jsbox *)ref)->v ;
	if (! JS_IsObject(obj)){
		return fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no methods") ;
	}
	enter(r) ;
	JSValue f ;
	int has = member(r, obj, method, &f) ;
	ugly_status st ;
	if (has < 0){
		st = js_failure(ctx, r) ;
	}
	else if (has == 0){
		st = fail(ctx, r, UGLY_ERR_NOT_FOUND, "ReferenceError", method) ;
	}
	else if (! JS_IsFunction(r->c, f)){
		JS_FreeValue(r->c, f) ;
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "member is not a method") ;
	}
	else {
		JSValue *a = js_values(ctx, r, args, nargs, &st) ;
		if (a != NULL){
			st = out_val(ctx, r, call_or_construct(r->c, f, obj, (int)nargs, a), want, out) ;
			free_values(r, a, nargs) ;
		}
		JS_FreeValue(r->c, f) ;
	}
	return leave(r, st) ;
}


static ugly_status js_new_instance(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_handle *out){

	jsrt *r = RT(rt) ;
	JSValueConst cls = ((jsbox *)ref)->v ;
	if (! JS_IsConstructor(r->c, cls)){
		return fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not a constructor") ;
	}
	enter(r) ;
	ugly_status st ;
	JSValue *a = js_values(ctx, r, args, nargs, &st) ;
	if (a != NULL){
		ugly_val v = ugly_null() ;
		st = out_val(ctx, r, JS_CallConstructor(r->c, cls, (int)nargs, a), UGLY_HANDLE, &v) ;
		if (st == UGLY_OK){
			*out = v.as.h ;
		}
		free_values(r, a, nargs) ;
	}
	return leave(r, st) ;
}


/* v[Symbol.iterator](): on an iterator, by JS's convention, itself. */
static ugly_status js_iter(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	jsrt *r = RT(rt) ;
	JSContext *c = r->c ;
	JSValueConst v = ((jsbox *)ref)->v ;
	if (! JS_IsObject(v)){
		return fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not iterable") ;
	}
	enter(r) ;
	JSValue f = JS_GetProperty(c, v, r->iterator) ;
	if (JS_IsException(f)){
		return leave(r, js_failure(ctx, r)) ;
	}
	if (! JS_IsFunction(c, f)){
		JS_FreeValue(c, f) ;
		return leave(r, fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not iterable")) ;
	}
	JSValue it = JS_Call(c, f, v, 0, NULL) ;
	JS_FreeValue(c, f) ;
	if ((! JS_IsException(it))&&(! JS_IsObject(it))){
		JS_FreeValue(c, it) ;
		it = JS_ThrowTypeError(c, "the Symbol.iterator method didn't return an object") ;
	}
	ugly_status st = (JS_IsException(it) ? js_failure(ctx, r) : export_val(ctx, r, it, out)) ;
	JS_FreeValue(c, it) ;
	return leave(r, st) ;
}


/*
 * it.next(), up to max times, until done: a generator runs only as far as
 * asked. Values made before an exception are kept.
 */
static ugly_status js_next(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_kind want, ugly_val *out, size_t max,
	size_t *got){

	jsrt *r = RT(rt) ;
	JSContext *c = r->c ;
	jsbox *b = ref ;
	*got = 0 ;
	if (! JS_IsObject(b->v)){
		return fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not an iterator") ;
	}
	enter(r) ;
	JSValue next = JS_GetPropertyStr(c, b->v, "next") ;
	if (JS_IsException(next)){
		return leave(r, js_failure(ctx, r)) ;
	}
	if (! JS_IsFunction(c, next)){
		JS_FreeValue(c, next) ;
		return leave(r, fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not an iterator")) ;
	}
	ugly_status st = UGLY_OK ;
	size_t n = 0 ;
	while (n < max){
		JSValue res = JS_Call(c, next, b->v, 0, NULL) ;
		if ((! JS_IsException(res))&&(! JS_IsObject(res))){
			JS_FreeValue(c, res) ;
			res = JS_ThrowTypeError(c, "iterator result is not an object") ;
		}
		if (JS_IsException(res)){
			b->open = 0 ;     /* a generator that threw is done */
			st = js_failure(ctx, r) ;
			break ;
		}
		JSValue done = JS_GetPropertyStr(c, res, "done") ;
		int finished = JS_ToBool(c, done) ;
		JS_FreeValue(c, done) ;
		if (finished){
			JS_FreeValue(c, res) ;
			b->open = 0 ;
			break ;
		}
		JSValue val = JS_GetPropertyStr(c, res, "value") ;
		JS_FreeValue(c, res) ;
		b->open = 1 ;
		st = out_val(ctx, r, val, want, &out[n]) ;
		if (st != UGLY_OK){
			break ;
		}
		n++ ;
	}
	JS_FreeValue(c, next) ;
	*got = n ;
	return leave(r, st) ;
}


/* String(v): toString(), through Symbol.toPrimitive if it has one. */
static ugly_status js_to_string(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_val *out){
	jsrt *r = RT(rt) ;
	enter(r) ;
	return leave(r, out_val(ctx, r, JS_ToString(r->c, ((jsbox *)ref)->v), UGLY_STRING, out)) ;
}


static int tearing_down ;     /* the binding's teardown, inside a QuickJS finalizer: no JS may run */

/*
 * The last reference to a value goes. An iterator stepped and not done is
 * closed first, as a break would: its return(), so a generator's finally
 * runs.
 */
static void js_release(ugly_rt *rt, void *ref){
	jsrt *r = RT(rt) ;
	jsbox *b = ref ;
	if (b->open && ! tearing_down){
		b->open = 0 ;
		enter(r) ;
		JSValue f = JS_GetPropertyStr(r->c, b->v, "return") ;
		if (JS_IsFunction(r->c, f)){
			JSValue x = JS_Call(r->c, f, b->v, 0, NULL) ;
			if (JS_IsException(x)){
				drop_exception(r->c) ;     /* nowhere to go, as for a break in a loop that's gone */
			}
			JS_FreeValue(r->c, x) ;
		}
		else if (JS_IsException(f)){
			drop_exception(r->c) ;
		}
		JS_FreeValue(r->c, f) ;
		leave(r, UGLY_OK) ;
	}
	free_box(r, b) ;     /* JS_FreeValueRT: works while the host frees its context */
}


/* ---------------------------------------------------------------- */
/* Lifecycle                                                        */
/* ---------------------------------------------------------------- */

static ugly_status js_init(ugly_ctx *ctx, const char *dir){
	(void)ctx ; (void)dir ;
	if (proxy_class == 0){
		JS_NewClassID(&proxy_class) ;
		JS_NewClassID(&proxy_fn_class) ;
		JS_NewClassID(&error_class) ;
		JS_NewClassID(&kw_class) ;
		JS_NewClassID(&opts_class) ;
	}
	return UGLY_OK ;
}


/* What every runtime needs, created or attached: our classes, prototypes, and an entry in the registry. */
static void setup(jsrt *r){
	JSContext *c = r->c ;
	if (! JS_IsRegisteredClass(r->rt, proxy_class)){
		JS_NewClass(r->rt, proxy_class, &proxy_def) ;
		JS_NewClass(r->rt, proxy_fn_class, &proxy_fn_def) ;
		JS_NewClass(r->rt, error_class, &error_def) ;
		JS_NewClass(r->rt, kw_class, &kw_def) ;
		JS_NewClass(r->rt, opts_class, &opts_def) ;
	}

	/* Ugly errors inherit Error.prototype, so they're Errors in JS (instanceof, stack). */
	JSValue global = JS_GetGlobalObject(c) ;
	JSValue error = JS_GetPropertyStr(c, global, "Error") ;
	JSValue error_proto = JS_GetPropertyStr(c, error, "prototype") ;
	r->error_proto = JS_NewObjectProto(c, error_proto) ;
	JS_FreeValue(c, error_proto) ;
	JS_FreeValue(c, error) ;
	r->array_buffer = JS_GetPropertyStr(c, global, "ArrayBuffer") ;
	JSValue symbol = JS_GetPropertyStr(c, global, "Symbol") ;
	JSValue tag = JS_GetPropertyStr(c, symbol, "toStringTag") ;
	r->to_string_tag = JS_ValueToAtom(c, tag) ;
	JS_FreeValue(c, tag) ;
	JSValue iterator = JS_GetPropertyStr(c, symbol, "iterator") ;
	r->iterator = JS_ValueToAtom(c, iterator) ;
	JS_FreeValue(c, iterator) ;

	/* The proxies' prototype (no other: a foreign value has no JS methods): iteration and toString. */
	JSValue proto = JS_NewObjectProto(c, JS_NULL) ;
	JS_SetProperty(c, proto, r->iterator, JS_NewCFunction(c, proxy_iterate, "[Symbol.iterator]", 0)) ;
	JS_SetPropertyStr(c, proto, "toString", JS_NewCFunction(c, proxy_to_string, "toString", 0)) ;
	JS_SetClassProto(c, proxy_class, JS_DupValue(c, proto)) ;
	JS_SetClassProto(c, proxy_fn_class, proto) ;
	JS_FreeValue(c, symbol) ;
	JS_FreeValue(c, global) ;

	r->next = runtimes ;
	runtimes = r ;
}


static ugly_status js_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	jsrt *r = calloc(1, sizeof(jsrt)) ;
	if (r == NULL){
		return UGLY_ERR_NOMEM ;
	}
	r->urt = rt ;
	r->owns = 1 ;
	r->rt = JS_NewRuntime() ;
	r->c = (r->rt != NULL ? JS_NewContext(r->rt) : NULL) ;
	if (r->c == NULL){
		if (r->rt != NULL){
			JS_FreeRuntime(r->rt) ;
		}
		free(r) ;
		return ugly_fail(ctx, rt, UGLY_ERR_NOMEM, "InternalError", "can't create a QuickJS runtime") ;
	}
	JS_SetModuleLoaderFunc(r->rt, NULL, module_loader, r) ;
	setup(r) ;
	*native = r ;
	return UGLY_OK ;
}


/*
 * An existing context: the host's, when a QuickJS program (qjs) loads the
 * binding. Its module loader, event loop and runtime opaque stay the host's.
 */
static ugly_status js_rt_attach(ugly_ctx *ctx, ugly_rt *rt, void *existing, void **native){
	(void)ctx ;
	if (existing == NULL){
		return UGLY_ERR_USAGE ;
	}
	jsrt *r = calloc(1, sizeof(jsrt)) ;
	if (r == NULL){
		return UGLY_ERR_NOMEM ;
	}
	r->urt = rt ;
	r->owns = 0 ;
	r->c = existing ;
	r->rt = JS_GetRuntime(r->c) ;
	setup(r) ;
	*native = r ;
	return UGLY_OK ;
}


static ugly_status js_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	(void)ctx ; (void)rt ;
	jsrt *r = native ;
	unregister(r) ;
	will_run++ ;
	JS_FreeValueRT(r->rt, r->error_proto) ;
	JS_FreeValueRT(r->rt, r->array_buffer) ;
	JS_FreeAtomRT(r->rt, r->to_string_tag) ;
	JS_FreeAtomRT(r->rt, r->iterator) ;
	if (r->owns){
		JS_FreeContext(r->c) ;
		JS_FreeRuntime(r->rt) ;     /* runs the remaining finalizers: proxies queue their handles */
	}
	run_later() ;     /* the engine is gone (or the host's): nothing of this runtime can run */
	will_run-- ;
	size_t i ;
	for (i = 0 ; i < sizeof(r->pcache) / sizeof(r->pcache[0]) ; i++){
		while (r->pcache[i] != NULL){
			pcache_entry *e = r->pcache[i] ;
			r->pcache[i] = e->next ;
			free(e) ;
		}
	}
	free(r) ;
	return UGLY_OK ;
}


static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = UGLY_CAP_MULTI_RUNTIME | UGLY_CAP_RESTARTABLE | UGLY_CAP_EVAL | UGLY_CAP_ATTACH,
	.name = "js",
	.init = js_init,
	.rt_new = js_rt_new,
	.options = (const char *const []){ "path", NULL },
	.rt_attach = js_rt_attach,
	.rt_delete = js_rt_delete,
	.import = js_import,
	.globals = js_globals,
	.eval = js_eval,
	.describe = js_describe,
	.get_member = js_get_member,
	.set_member = js_set_member,
	.member_names = js_member_names,
	.length = js_length,
	.get_index = js_get_index,
	.set_index = js_set_index,
	.delete_member = js_delete_member,
	.splice = js_splice,
	.call = js_call,
	.invoke = js_invoke,
	.new_instance = js_new_instance,
	.iter = js_iter,
	.next = js_next,
	.to_string = js_to_string,
	.release = js_release,
} ;


const ugly_plugin *ugly_plugin_js(void){
	return &plugin ;
}


#ifdef UGLY_BINDING
/* ---------------------------------------------------------------- */
/* The binding: import "ugly.so" from a plain qjs program           */
/* ---------------------------------------------------------------- */

/*
 * Built from this same file as ugly.so, without QuickJS: its engine symbols
 * come from qjs (linked -rdynamic). qjs loads it as a native ES module and
 * calls js_init_module. It registers this plugin as "js", attaches to qjs's
 * context, and returns the same ugly module embedded runtimes get.
 *
 * Teardown must happen before qjs's JS_FreeRuntime, which asserts that no
 * object is still referenced, and other languages may hold JS values when
 * qjs exits. QuickJS has no runtime-finalizer hook, so a sentinel object on
 * the global object holds the teardown, and declares every reference the
 * binding holds (the boxes behind handles, the cached prototype and
 * constructor) to the GC through gc_mark. While the sentinel is reachable,
 * so is everything it holds. When qjs frees its context, the whole graph
 * becomes garbage together, the sentinel's finalizer tears down, and it
 * releases exactly the references gc_mark declared, as QuickJS expects of a
 * finalizer.
 */
static ugly_ctx *b_ctx ;
static ugly_lang *b_lang ;
static ugly_rt *b_rt ;
static int b_owns_ctx ;
static JSClassID binding_class ;

static void binding_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func){
	(void)val ;
	jsrt *r = (b_rt != NULL ? ugly_rt_native(b_rt) : NULL) ;
	if (r == NULL){
		return ;
	}
	jsbox *b ;
	for (b = r->boxes ; b != NULL ; b = b->next){
		JS_MarkValue(rt, b->v, mark_func) ;
	}
	JS_MarkValue(rt, r->error_proto, mark_func) ;
	JS_MarkValue(rt, r->array_buffer, mark_func) ;
}


static void binding_finalizer(JSRuntime *rt, JSValue val){
	(void)rt ; (void)val ;
	if (b_rt != NULL){
		tearing_down = 1 ;
		ugly_rt_delete(b_ctx, b_rt) ;
		ugly_lang_unload(b_ctx, b_lang) ;
		if (b_owns_ctx){
			ugly_ctx_delete(b_ctx) ;     /* also deletes the runtimes ugly.module created */
		}
		b_rt = NULL ;
		run_later() ;     /* what this context's proxies queued: no runtime is left for a release to enter */
		will_run-- ;
		tearing_down = 0 ;
	}
}

static JSClassDef binding_def = { "ugly.Binding", .finalizer = binding_finalizer, .gc_mark = binding_mark } ;


JSModuleDef *js_init_module(JSContext *c, const char *module_name){
	if (b_rt == NULL){
		b_ctx = ugly_current_ctx() ;
		if (b_ctx == NULL){
			if (ugly_ctx_new(&b_ctx) != UGLY_OK){
				JS_ThrowInternalError(c, "ugly: can't create a context") ;
				return NULL ;
			}
			b_owns_ctx = 1 ;
		}
		ugly_status st = ugly_lang_register(b_ctx, &plugin, &b_lang) ;
		if (st == UGLY_OK){
			st = ugly_rt_attach(b_ctx, b_lang, c, &b_rt) ;
		}
		if (st != UGLY_OK){
			const ugly_error *e = ugly_last_error(b_ctx) ;
			JS_ThrowInternalError(c, "ugly: can't attach: %s", e ? ugly_error_message(e) : "?") ;
			return NULL ;
		}
		will_run++ ;     /* until the teardown: finalizers queue their releases (release_later) */
		JSRuntime *rt = JS_GetRuntime(c) ;
		JS_NewClassID(&binding_class) ;
		JS_NewClass(rt, binding_class, &binding_def) ;
		JSValue global = JS_GetGlobalObject(c) ;
		JS_DefinePropertyValueStr(c, global, "__ugly_binding", JS_NewObjectClass(c, (int)binding_class), 0) ;
		JS_FreeValue(c, global) ;
	}
	JSModuleDef *m = JS_NewCModule(c, module_name, api_init) ;
	if (m != NULL){
		JS_AddModuleExportList(c, m, api, sizeof(api) / sizeof(api[0])) ;
	}
	return m ;
}
#endif
