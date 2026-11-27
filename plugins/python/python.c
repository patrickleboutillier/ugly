/*
 * The CPython plugin (3.10+).
 *
 * - Python is initialized once per process, at plugin load (unless the host
 *   process already runs Python), with Py_InitializeEx(0) so it doesn't take
 *   over signals. It is never finalized: Py_FinalizeEx followed by a new
 *   Py_Initialize isn't reliable with extension modules. So the plugin isn't
 *   UGLY_CAP_RESTARTABLE, and has one runtime at a time (no
 *   UGLY_CAP_MULTI_RUNTIME): a runtime is a globals namespace.
 * - Every entry takes the GIL (PyGILState_Ensure/Release), which also works
 *   when calls nest through other languages back into Python.
 * - Native refs are PyObject* holding one reference; identity is the
 *   object's address.
 * - The embedded "ugly" module holds ugly.Proxy (foreign values) and
 *   ugly.Error (foreign errors, with .type, .message, .language and
 *   .payload). An ugly.Error escaping back out of Python restores the
 *   original error unchanged.
 * - Conversions follow Python's own rules: no implicit str <-> number, no
 *   float -> int; ints beyond int64 are UGLY_ERR_TYPE.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pthread.h>
#include <string.h>

#include "ugly_plugin.h"


typedef struct pyrt {
	ugly_rt *urt ;
	PyObject *globals ;
} pyrt ;

static pyrt *current_rt ;        /* the one runtime */
static PyObject *ugly_module ;
static PyObject *ErrorType ;
static PyObject *OptsType ;      /* ugly.Opts: ugly.opts(hint=..., context=...), passed last */
static PyObject *proxy_cache ;   /* owned handle -> weakref(proxy) */
static PyTypeObject ProxyType ;

typedef struct {
	PyObject_HEAD
	ugly_handle h ;              /* one owned reference */
	PyObject *weakrefs ;
	int traits ;                 /* the value's traits, once asked for (proxy_call); -1 until then */
} proxy ;

#define ENTER PyGILState_STATE gil_ = PyGILState_Ensure()
#define LEAVE PyGILState_Release(gil_)
#define ERR_CAPSULE "ugly.error"


static ugly_rt *URT(void){
	return (current_rt != NULL ? current_rt->urt : NULL) ;
}


/* ---------------------------------------------------------------- */
/* Errors: Python exceptions <-> ugly_errors                        */
/* ---------------------------------------------------------------- */

static void capsule_free(PyObject *cap){
	ugly_error_free(PyCapsule_GetPointer(cap, ERR_CAPSULE)) ;
}


static PyObject *from_val(ugly_ctx *ctx, ugly_val v) ;

/*
 * Raises the ctx's current error in Python, as ugly.Error, and returns NULL.
 * If the failure recorded no error, records one with 'st' first.
 */
static PyObject *raise_ugly(ugly_ctx *ctx, ugly_status st){
	if ((ugly_last_error(ctx) == NULL)&&(URT() != NULL)){
		ugly_fail(ctx, URT(), st, "Error", "operation failed") ;
	}
	ugly_error *err = ugly_take_error(ctx) ;
	if (err == NULL){
		PyErr_SetString(PyExc_RuntimeError, "ugly: operation failed") ;
		return NULL ;
	}

	/* A Python exception coming home (rule 7): raise the original object itself. */
	ugly_handle home = UGLY_NO_HANDLE ;
	void *ref = NULL ;
	if ((URT() != NULL)&&(ugly_error_payload(ctx, err, &home) == UGLY_OK)&&(home != UGLY_NO_HANDLE)
		&&(ugly_resolve(ctx, URT(), home, &ref) == UGLY_OK)&&(ref != NULL)&&(PyExceptionInstance_Check((PyObject *)ref))){
		PyErr_SetObject((PyObject *)Py_TYPE((PyObject *)ref), (PyObject *)ref) ;
		ugly_release(ctx, home) ;
		ugly_error_free(err) ;
		return NULL ;
	}
	ugly_release(ctx, home) ;

	PyObject *exc = PyObject_CallFunction(ErrorType, "s", ugly_error_message(err)) ;
	if (exc == NULL){
		ugly_error_free(err) ;
		return NULL ;
	}
	PyObject *s ;
	s = PyUnicode_FromString(ugly_error_type(err)) ; PyObject_SetAttrString(exc, "type", s) ; Py_XDECREF(s) ;
	s = PyUnicode_FromString(ugly_error_message(err)) ; PyObject_SetAttrString(exc, "message", s) ; Py_XDECREF(s) ;
	s = PyUnicode_FromString(ugly_error_language(err)) ; PyObject_SetAttrString(exc, "language", s) ; Py_XDECREF(s) ;

	ugly_handle h = UGLY_NO_HANDLE ;
	PyObject *payload = NULL ;
	if ((ugly_error_payload(ctx, err, &h) == UGLY_OK)&&(h != UGLY_NO_HANDLE)){
		payload = from_val(ctx, ugly_hval(h)) ;     /* ours: the original object; foreign: a proxy */
		ugly_release(ctx, h) ;
		if (payload == NULL){
			PyErr_Clear() ;
		}
	}
	PyObject_SetAttrString(exc, "payload", payload != NULL ? payload : Py_None) ;
	Py_XDECREF(payload) ;

	PyObject *cap = PyCapsule_New(err, ERR_CAPSULE, capsule_free) ;
	PyObject_SetAttrString(exc, "_err", cap) ;
	Py_XDECREF(cap) ;
	PyErr_SetObject(ErrorType, exc) ;
	Py_DECREF(exc) ;
	return NULL ;
}


static ugly_status export_obj(ugly_ctx *ctx, PyObject *o, ugly_handle *out) ;

/* The Python exception that is set, escaping to the core (rule 7). Clears it. */
static ugly_status py_failure(ugly_ctx *ctx){
	PyObject *t, *v, *tb ;
	PyErr_Fetch(&t, &v, &tb) ;
	PyErr_NormalizeException(&t, &v, &tb) ;
	ugly_status st ;

	if ((v != NULL)&&(PyErr_GivenExceptionMatches(v, ErrorType))){
		/* Our own wrapper, not yet consumed: the original error, unchanged. */
		PyObject *cap = PyObject_GetAttrString(v, "_err") ;
		if ((cap != NULL)&&(PyCapsule_CheckExact(cap))&&(PyCapsule_GetDestructor(cap) != NULL)){
			ugly_error *err = PyCapsule_GetPointer(cap, ERR_CAPSULE) ;
			PyCapsule_SetDestructor(cap, NULL) ;
			Py_DECREF(cap) ;
			Py_XDECREF(t) ; Py_XDECREF(v) ; Py_XDECREF(tb) ;
			return ugly_restore_error(ctx, err) ;
		}
		Py_XDECREF(cap) ;
		PyErr_Clear() ;

		/* Raised again after it was consumed, or ugly.rethrow(foreign): the payload is the foreign value. */
		PyObject *payload = PyObject_GetAttrString(v, "payload") ;
		PyErr_Clear() ;
		if ((payload != NULL)&&(PyObject_TypeCheck(payload, &ProxyType))){
			ugly_handle h ;
			st = ugly_dup(ctx, ((proxy *)payload)->h, &h) ;
			PyObject *ty = PyObject_GetAttrString(v, "type") ;
			PyErr_Clear() ;
			const char *type = ((ty != NULL)&&(PyUnicode_Check(ty)) ? PyUnicode_AsUTF8(ty) : "ForeignError") ;
			st = ugly_raise_handle(ctx, URT(), type, "rethrown foreign exception", (st == UGLY_OK ? h : UGLY_NO_HANDLE)) ;
			Py_XDECREF(ty) ;
			Py_DECREF(payload) ;
			Py_XDECREF(t) ; Py_XDECREF(v) ; Py_XDECREF(tb) ;
			return st ;
		}
		Py_XDECREF(payload) ;
	}

	/* A Python exception: type name, str(), and the exception object as payload. */
	char type[128] = "Exception" ;
	if (t != NULL){
		const char *n = ((PyTypeObject *)t)->tp_name ;
		const char *dot = strrchr(n, '.') ;
		snprintf(type, sizeof(type), "%s", dot != NULL ? dot + 1 : n) ;
	}
	char message[512] = "" ;
	if (v != NULL){
		PyObject *s = PyObject_Str(v) ;
		if ((s != NULL)&&(PyUnicode_AsUTF8(s) != NULL)){
			snprintf(message, sizeof(message), "%s", PyUnicode_AsUTF8(s)) ;
		}
		Py_XDECREF(s) ;
		PyErr_Clear() ;
	}
	if (v != NULL){
		Py_INCREF(v) ;     /* the payload's reference */
		st = ugly_raise_native(ctx, URT(), type, message, v, (uintptr_t)v) ;
	}
	else {
		st = ugly_raise_native(ctx, URT(), type, message, NULL, 0) ;
	}
	Py_XDECREF(t) ; Py_XDECREF(v) ; Py_XDECREF(tb) ;
	return st ;
}


/* For failures that aren't exceptions: the status, Python's type name for it, no payload. */
static ugly_status fail(ugly_ctx *ctx, ugly_status st, const char *type, const char *msg){
	PyErr_Clear() ;
	return ugly_fail(ctx, URT(), st, type, msg) ;
}


/* ---------------------------------------------------------------- */
/* Python -> ugly_val                                               */
/* ---------------------------------------------------------------- */

static ugly_status export_obj(ugly_ctx *ctx, PyObject *o, ugly_handle *out){
	if (PyObject_TypeCheck(o, &ProxyType)){
		return ugly_dup(ctx, ((proxy *)o)->h, out) ;     /* rule 5: unwrap */
	}
	Py_INCREF(o) ;
	return ugly_export(ctx, URT(), o, (uintptr_t)o, out) ;
}


static ugly_status type_error(ugly_ctx *ctx, PyObject *o, const char *to){
	char msg[160] ;
	snprintf(msg, sizeof(msg), "can't convert a Python %s to %s", Py_TYPE(o)->tp_name, to) ;
	return fail(ctx, UGLY_ERR_TYPE, "TypeError", msg) ;
}


/* A Python object as a result of kind 'want', by Python's own conversion rules. */
static ugly_status to_val(ugly_ctx *ctx, PyObject *o, ugly_kind want, ugly_val *out){
	*out = ugly_null() ;
	if (want == UGLY_ANY){
		if (o == Py_None) return UGLY_OK ;
		else if (PyBool_Check(o)) want = UGLY_BOOL ;
		else if (PyLong_Check(o)) want = UGLY_INT ;
		else if (PyFloat_Check(o)) want = UGLY_DOUBLE ;
		else if (PyUnicode_Check(o)) want = UGLY_STRING ;
		else if (PyBytes_Check(o)||PyByteArray_Check(o)) want = UGLY_BYTES ;
		else want = UGLY_HANDLE ;
	}
	switch (want){
		case UGLY_NULL:
			return (o == Py_None ? UGLY_OK : type_error(ctx, o, "null")) ;
		case UGLY_BOOL: {
			int b = PyObject_IsTrue(o) ;     /* Python's truthiness */
			if (b < 0){
				return py_failure(ctx) ;
			}
			*out = ugly_bool(b) ;
			return UGLY_OK ;
		}
		case UGLY_INT: {
			if (! PyIndex_Check(o)){
				return type_error(ctx, o, "an int") ;     /* no implicit float or str -> int */
			}
			PyObject *i = PyNumber_Index(o) ;
			if (i == NULL){
				return py_failure(ctx) ;
			}
			int overflow = 0 ;
			long long v = PyLong_AsLongLongAndOverflow(i, &overflow) ;
			Py_DECREF(i) ;
			if (overflow != 0){
				return fail(ctx, UGLY_ERR_TYPE, "OverflowError", "int doesn't fit in int64") ;
			}
			if ((v == -1)&&(PyErr_Occurred())){
				return py_failure(ctx) ;
			}
			*out = ugly_int(v) ;
			return UGLY_OK ;
		}
		case UGLY_DOUBLE: {
			if (PyUnicode_Check(o)||PyBytes_Check(o)){
				return type_error(ctx, o, "a double") ;
			}
			double d = PyFloat_AsDouble(o) ;     /* floats, ints, and anything with __float__ */
			if ((d == -1.0)&&(PyErr_Occurred())){
				return type_error(ctx, o, "a double") ;
			}
			*out = ugly_double(d) ;
			return UGLY_OK ;
		}
		case UGLY_STRING: {
			if (! PyUnicode_Check(o)){
				return type_error(ctx, o, "a string") ;     /* no implicit str() */
			}
			Py_ssize_t len ;
			const char *s = PyUnicode_AsUTF8AndSize(o, &len) ;
			if (s == NULL){
				return fail(ctx, UGLY_ERR_TYPE, "UnicodeEncodeError", "string isn't valid Unicode (lone surrogate?)") ;
			}
			return ugly_val_set_string(ctx, out, s, (size_t)len) ;
		}
		case UGLY_BYTES: {
			if (PyBytes_Check(o)){
				return ugly_val_set_bytes(ctx, out, (const uint8_t *)PyBytes_AS_STRING(o), (size_t)PyBytes_GET_SIZE(o)) ;
			}
			if (PyByteArray_Check(o)){
				return ugly_val_set_bytes(ctx, out, (const uint8_t *)PyByteArray_AS_STRING(o), (size_t)PyByteArray_GET_SIZE(o)) ;
			}
			return type_error(ctx, o, "bytes") ;
		}
		case UGLY_HANDLE: {
			if ((o == Py_None)||PyBool_Check(o)||PyLong_Check(o)||PyFloat_Check(o)||PyUnicode_Check(o)||PyBytes_Check(o)){
				return type_error(ctx, o, "a handle") ;
			}
			ugly_handle h ;
			ugly_status st = export_obj(ctx, o, &h) ;
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
/* ugly_val -> Python                                               */
/* ---------------------------------------------------------------- */

static PyObject *proxy_for(ugly_ctx *ctx, ugly_handle borrowed){
	ugly_handle h ;
	ugly_status st = ugly_dup(ctx, borrowed, &h) ;     /* owned: the same id for the same value */
	if (st != UGLY_OK){
		return raise_ugly(ctx, st) ;
	}
	PyObject *key = PyLong_FromUnsignedLongLong(h) ;
	PyObject *ref = PyDict_GetItemWithError(proxy_cache, key) ;
	if (ref != NULL){
		PyObject *p = PyWeakref_GetObject(ref) ;
		if (p != Py_None){
			Py_INCREF(p) ;
			Py_DECREF(key) ;
			ugly_release(ctx, h) ;     /* the cached proxy holds its own reference */
			return p ;
		}
	}
	proxy *p = PyObject_New(proxy, &ProxyType) ;
	if (p == NULL){
		Py_DECREF(key) ;
		ugly_release(ctx, h) ;
		return NULL ;
	}
	p->h = h ;
	p->weakrefs = NULL ;
	p->traits = -1 ;
	PyObject *w = PyWeakref_NewRef((PyObject *)p, NULL) ;
	if (w != NULL){
		PyDict_SetItem(proxy_cache, key, w) ;
		Py_DECREF(w) ;
	}
	PyErr_Clear() ;
	Py_DECREF(key) ;
	return (PyObject *)p ;
}


/* A new reference for an argument (borrowed): ours resolve to the original (rule 5), foreign ones get a proxy. NULL with an exception on failure. */
static PyObject *from_val(ugly_ctx *ctx, ugly_val v){
	switch (v.kind){
		case UGLY_NULL: Py_RETURN_NONE ;
		case UGLY_BOOL: return PyBool_FromLong(v.as.b) ;
		case UGLY_INT: return PyLong_FromLongLong(v.as.i) ;
		case UGLY_DOUBLE: return PyFloat_FromDouble(v.as.d) ;
		case UGLY_STRING: return PyUnicode_DecodeUTF8(v.as.str.ptr, (Py_ssize_t)v.as.str.len, "strict") ;
		case UGLY_BYTES: return PyBytes_FromStringAndSize((const char *)v.as.bytes.ptr, (Py_ssize_t)v.as.bytes.len) ;
		case UGLY_HANDLE: {
			void *ref = NULL ;
			ugly_status st = ugly_resolve(ctx, URT(), v.as.h, &ref) ;
			if (st != UGLY_OK){
				return raise_ugly(ctx, st) ;
			}
			if (ref != NULL){
				Py_INCREF((PyObject *)ref) ;
				return ref ;
			}
			return proxy_for(ctx, v.as.h) ;
		}
		default:
			PyErr_SetString(PyExc_ValueError, "ugly: invalid value") ;
			return NULL ;
	}
}


static PyObject *args_tuple(ugly_ctx *ctx, const ugly_val *args, size_t n){
	PyObject *t = PyTuple_New((Py_ssize_t)n) ;
	size_t i ;
	for (i = 0 ; (t != NULL)&&(i < n) ; i++){
		PyObject *a = from_val(ctx, args[i]) ;
		if (a == NULL){
			Py_DECREF(t) ;
			return NULL ;
		}
		PyTuple_SET_ITEM(t, (Py_ssize_t)i, a) ;
	}
	return t ;
}


/* ---------------------------------------------------------------- */
/* ugly.Proxy                                                       */
/* ---------------------------------------------------------------- */

/*
 * A call out of Python, through the core into another language. The GIL is
 * released meanwhile: other threads run Python while the other language
 * works, and a callback into Python from another thread can't deadlock on
 * it (it takes the GIL back with PyGILState_Ensure). Arguments are converted
 * before, results after, with the GIL held.
 */
#define OUT(call) ({ ugly_status out_st_ ; Py_BEGIN_ALLOW_THREADS out_st_ = (call) ; Py_END_ALLOW_THREADS out_st_ ; })


/* This thread's ctx; a Python thread that has none gets one, deleted when it ends (ugly_ctx_thread). */
static ugly_ctx *need_ctx(void){
	ugly_ctx *ctx = NULL ;
	if (ugly_ctx_thread(&ctx) != UGLY_OK){
		PyErr_SetString(PyExc_RuntimeError, "ugly: can't create a ctx on this thread") ;
	}
	return ctx ;
}


/* Python args -> owned ugly_vals. Returns the count made; -1 (exception set) on failure. */
static Py_ssize_t py_args(ugly_ctx *ctx, PyObject *tuple, ugly_val **out){
	Py_ssize_t n = PyTuple_GET_SIZE(tuple), i ;
	*out = calloc((size_t)n + 1, sizeof(ugly_val)) ;
	for (i = 0 ; i < n ; i++){
		ugly_status st = to_val(ctx, PyTuple_GET_ITEM(tuple, i), UGLY_ANY, &(*out)[i]) ;
		if (st != UGLY_OK){
			Py_ssize_t j ;
			for (j = 0 ; j < i ; j++){
				ugly_val_release(ctx, &(*out)[j]) ;
			}
			free(*out) ;
			*out = NULL ;
			raise_ugly(ctx, st) ;
			return -1 ;
		}
	}
	return n ;
}


static void free_args(ugly_ctx *ctx, ugly_val *args, Py_ssize_t n){
	Py_ssize_t i ;
	for (i = 0 ; i < n ; i++){
		ugly_val_release(ctx, &args[i]) ;
	}
	free(args) ;
}


/* Turns a core result into a Python object (and releases it), or raises ugly.Error. */
static PyObject *result(ugly_ctx *ctx, ugly_status st, ugly_val *out){
	if (st != UGLY_OK){
		return raise_ugly(ctx, st) ;
	}
	PyObject *r = from_val(ctx, *out) ;
	ugly_val_release(ctx, out) ;
	return r ;
}


/*
 * Python keyword arguments -> call options (contract §11): names point into
 * the dict's keys, which the caller holds; values are owned (free_kwargs).
 * 0, or -1 with an exception set.
 */
static int py_kwargs(ugly_ctx *ctx, PyObject *kw, ugly_opts *o, ugly_kwarg **out){
	*o = (ugly_opts)UGLY_OPTS_INIT ;
	*out = NULL ;
	if ((kw == NULL)||(PyDict_Size(kw) == 0)){
		return 0 ;
	}
	Py_ssize_t n = PyDict_Size(kw), pos = 0, i = 0 ;
	*out = calloc((size_t)n, sizeof(ugly_kwarg)) ;
	PyObject *key, *value ;
	while (PyDict_Next(kw, &pos, &key, &value)){
		const char *name = PyUnicode_AsUTF8(key) ;
		ugly_status st = (name != NULL ? to_val(ctx, value, UGLY_ANY, &(*out)[i].value) : UGLY_ERR_TYPE) ;
		if (st != UGLY_OK){
			while (i-- > 0){
				ugly_val_release(ctx, &(*out)[i].value) ;
			}
			free(*out) ;
			*out = NULL ;
			if (! PyErr_Occurred()){
				raise_ugly(ctx, st) ;
			}
			return -1 ;
		}
		(*out)[i++].name = name ;
	}
	o->kwargs = *out ;
	o->nkwargs = (size_t)i ;
	return 0 ;
}


/*
 * ugly.opts(hint=..., context=...) as the last positional argument: the
 * call's hint and context (contract §11). Returns the arguments without it
 * (a new reference); *hold keeps the option strings alive for the call.
 */
static PyObject *take_opts(PyObject *args, ugly_opts *o, PyObject **hold){
	*hold = NULL ;
	Py_ssize_t n = PyTuple_GET_SIZE(args) ;
	if ((n == 0)||(OptsType == NULL)||(PyObject_IsInstance(PyTuple_GET_ITEM(args, n - 1), OptsType) != 1)){
		Py_INCREF(args) ;
		return args ;
	}
	PyObject *opts = PyTuple_GET_ITEM(args, n - 1) ;
	PyObject *hint = PyObject_GetAttrString(opts, "hint") ;
	PyObject *context = PyObject_GetAttrString(opts, "context") ;
	if ((hint == NULL)||(context == NULL)){
		Py_XDECREF(hint) ;
		Py_XDECREF(context) ;
		return NULL ;
	}
	o->hint = (PyUnicode_Check(hint) ? PyUnicode_AsUTF8(hint) : NULL) ;
	o->context = (PyUnicode_Check(context) ? PyUnicode_AsUTF8(context) : NULL) ;
	*hold = PyTuple_Pack(2, hint, context) ;
	Py_DECREF(hint) ;
	Py_DECREF(context) ;
	return PyTuple_GetSlice(args, 0, n - 1) ;
}


/* ugly.opts(hint=None, context=None): call options for the last argument of a foreign call. */
static PyObject *mod_opts(PyObject *mod, PyObject *args, PyObject *kwargs){
	(void)mod ;
	static char *names[] = { "hint", "context", NULL } ;
	PyObject *hint = Py_None, *context = Py_None ;
	if (! PyArg_ParseTupleAndKeywords(args, kwargs, "|$OO:opts", names, &hint, &context)){
		return NULL ;
	}
	if (((hint != Py_None)&&(! PyUnicode_Check(hint)))||((context != Py_None)&&(! PyUnicode_Check(context)))){
		PyErr_SetString(PyExc_TypeError, "ugly.opts: hint and context are strings") ;
		return NULL ;
	}
	PyObject *o = PyObject_CallNoArgs(OptsType) ;
	if ((o != NULL)&&((PyObject_SetAttrString(o, "hint", hint) != 0)||(PyObject_SetAttrString(o, "context", context) != 0))){
		Py_CLEAR(o) ;
	}
	return o ;
}


static void free_kwargs(ugly_ctx *ctx, ugly_opts *o, ugly_kwarg *kw){
	size_t i ;
	for (i = 0 ; i < o->nkwargs ; i++){
		ugly_val_release(ctx, &kw[i].value) ;
	}
	free(kw) ;
}


/* The value's traits, asked for once (a value's traits don't change). */
static int proxy_traits(ugly_ctx *ctx, proxy *p){
	if (p->traits < 0){
		uint32_t t = 0 ;
		ugly_val tn = ugly_null() ;
		p->traits = ((OUT(ugly_describe(ctx, p->h, &t, &tn)) == UGLY_OK) ? (int)t : 0) ;
		ugly_val_release(ctx, &tn) ;
	}
	return p->traits ;
}


static PyObject *proxy_call(PyObject *self, PyObject *args, PyObject *kw){
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return NULL ;
	}
	ugly_opts o ;
	ugly_kwarg *k ;
	if (py_kwargs(ctx, kw, &o, &k) != 0){
		return NULL ;
	}
	PyObject *hold ;
	args = take_opts(args, &o, &hold) ;
	if (args == NULL){
		free_kwargs(ctx, &o, k) ;
		return NULL ;
	}
	ugly_val *a ;
	Py_ssize_t n = py_args(ctx, args, &a) ;
	Py_DECREF(args) ;
	if (n < 0){
		free_kwargs(ctx, &o, k) ;
		Py_XDECREF(hold) ;
		return NULL ;
	}
	/*
	 * Calling a class that can't be called but can be instantiated (a Perl
	 * package, a Lua or Java class) constructs it, as calling a class does
	 * in Python: Point(3, 4), KwBox(value=5).
	 */
	proxy *p = (proxy *)self ;
	proxy_traits(ctx, p) ;
	ugly_val out = ugly_null() ;
	ugly_status st ;
	if ((p->traits & UGLY_TRAIT_INSTANTIABLE)&&(! (p->traits & UGLY_TRAIT_CALLABLE))){
		ugly_handle h = UGLY_NO_HANDLE ;
		st = OUT(ugly_new_opts(ctx, p->h, a, (size_t)n, &o, &h)) ;
		out = ugly_hval(h) ;
	}
	else {
		st = OUT(ugly_call_opts(ctx, p->h, a, (size_t)n, &o, UGLY_ANY, &out)) ;
	}
	free_args(ctx, a, n) ;
	free_kwargs(ctx, &o, k) ;
	Py_XDECREF(hold) ;
	return result(ctx, st, &out) ;
}


/* A bound foreign method: self is (proxy, name). */
static PyObject *invoke_bound(PyObject *bound, PyObject *args, PyObject *kw){
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return NULL ;
	}
	proxy *p = (proxy *)PyTuple_GET_ITEM(bound, 0) ;
	const char *name = PyUnicode_AsUTF8(PyTuple_GET_ITEM(bound, 1)) ;
	ugly_opts o ;
	ugly_kwarg *k ;
	if (py_kwargs(ctx, kw, &o, &k) != 0){
		return NULL ;
	}
	PyObject *hold ;
	args = take_opts(args, &o, &hold) ;
	if (args == NULL){
		free_kwargs(ctx, &o, k) ;
		return NULL ;
	}
	ugly_val *a ;
	Py_ssize_t n = py_args(ctx, args, &a) ;
	Py_DECREF(args) ;
	if (n < 0){
		free_kwargs(ctx, &o, k) ;
		Py_XDECREF(hold) ;
		return NULL ;
	}
	ugly_val out = ugly_null() ;
	ugly_status st = OUT(ugly_invoke_opts(ctx, p->h, name, a, (size_t)n, &o, UGLY_ANY, &out)) ;
	free_args(ctx, a, n) ;
	free_kwargs(ctx, &o, k) ;
	Py_XDECREF(hold) ;
	return result(ctx, st, &out) ;
}

static PyMethodDef invoke_def = { "method", (PyCFunction)(void (*)(void))invoke_bound, METH_VARARGS | METH_KEYWORDS,
	"a method of a foreign object" } ;


static PyObject *bound_method(PyObject *self, PyObject *name){
	PyObject *bound = PyTuple_Pack(2, self, name) ;
	if (bound == NULL){
		return NULL ;
	}
	PyObject *f = PyCFunction_New(&invoke_def, bound) ;
	Py_DECREF(bound) ;
	return f ;
}


static PyObject *proxy_getattro(PyObject *self, PyObject *name){
	const char *n = PyUnicode_AsUTF8(name) ;
	if (n == NULL){
		return NULL ;
	}
	if ((n[0] == '_')&&(n[1] == '_')){
		return PyObject_GenericGetAttr(self, name) ;
	}
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return NULL ;
	}
	ugly_val out = ugly_null() ;
	ugly_status st = OUT(ugly_get_member(ctx, ((proxy *)self)->h, n, UGLY_ANY, &out)) ;
	if ((st == UGLY_ERR_NOT_FOUND)||(st == UGLY_ERR_NOT_CAPABLE)){
		return bound_method(self, name) ;     /* not a member: a method of the foreign object */
	}
	if ((st == UGLY_OK)&&(out.kind == UGLY_HANDLE)){
		/* A callable member is a method: obj.m(...) invokes it, with the foreign receiver rules. */
		uint32_t traits = 0 ;
		ugly_val tn ;
		if ((OUT(ugly_describe(ctx, out.as.h, &traits, &tn)) == UGLY_OK)&&(traits & UGLY_TRAIT_CALLABLE)){
			ugly_val_release(ctx, &tn) ;
			ugly_val_release(ctx, &out) ;
			return bound_method(self, name) ;
		}
		ugly_val_release(ctx, &tn) ;
	}
	return result(ctx, st, &out) ;
}


static int proxy_setattro(PyObject *self, PyObject *name, PyObject *value){
	ugly_ctx *ctx = need_ctx() ;
	const char *n = PyUnicode_AsUTF8(name) ;
	if ((ctx == NULL)||(n == NULL)){
		return -1 ;
	}
	if (value == NULL){
		PyErr_SetString(PyExc_TypeError, "ugly: can't delete a foreign member") ;
		return -1 ;
	}
	ugly_val v ;
	ugly_status st = to_val(ctx, value, UGLY_ANY, &v) ;
	if (st == UGLY_OK){
		st = OUT(ugly_set_member(ctx, ((proxy *)self)->h, n, v)) ;
		ugly_val_release(ctx, &v) ;
	}
	if (st != UGLY_OK){
		raise_ugly(ctx, st) ;
		return -1 ;
	}
	return 0 ;
}


static PyObject *proxy_item(PyObject *self, Py_ssize_t i){
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return NULL ;
	}
	ugly_val out = ugly_null() ;
	ugly_status st = OUT(ugly_get_index(ctx, ((proxy *)self)->h, (int64_t)i, UGLY_ANY, &out)) ;
	if (st == UGLY_ERR_NOT_FOUND){
		PyErr_SetString(PyExc_IndexError, "index out of range") ;     /* also ends iteration */
		return NULL ;
	}
	return result(ctx, st, &out) ;
}


static PyObject *proxy_subscript(PyObject *self, PyObject *key){
	if (PyIndex_Check(key)){
		Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError) ;
		if ((i == -1)&&(PyErr_Occurred())){
			return NULL ;
		}
		return proxy_item(self, i) ;
	}
	const char *k = PyUnicode_Check(key) ? PyUnicode_AsUTF8(key) : NULL ;
	ugly_ctx *ctx = need_ctx() ;
	if ((k == NULL)||(ctx == NULL)){
		if (! PyErr_Occurred()){
			PyErr_SetString(PyExc_TypeError, "ugly: keys are ints (elements) or strs (members)") ;
		}
		return NULL ;
	}
	ugly_val out = ugly_null() ;
	ugly_status st = OUT(ugly_get_member(ctx, ((proxy *)self)->h, k, UGLY_ANY, &out)) ;
	if (st == UGLY_ERR_NOT_FOUND){
		PyErr_SetObject(PyExc_KeyError, key) ;
		return NULL ;
	}
	return result(ctx, st, &out) ;
}


static int proxy_ass_subscript(PyObject *self, PyObject *key, PyObject *value){
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return -1 ;
	}
	if (value == NULL){
		PyErr_SetString(PyExc_TypeError, "ugly: can't delete a foreign element") ;
		return -1 ;
	}
	/* The key first, with the GIL: OUT() releases it around its whole argument. */
	Py_ssize_t index = 0 ;
	const char *name = NULL ;
	if (PyIndex_Check(key)){
		index = PyNumber_AsSsize_t(key, PyExc_IndexError) ;
		if ((index == -1)&&PyErr_Occurred()){
			return -1 ;
		}
	}
	else if (PyUnicode_Check(key)){
		if ((name = PyUnicode_AsUTF8(key)) == NULL){
			return -1 ;
		}
	}
	else {
		PyErr_SetString(PyExc_TypeError, "ugly: keys are ints (elements) or strs (members)") ;
		return -1 ;
	}
	ugly_val v ;
	ugly_status st = to_val(ctx, value, UGLY_ANY, &v) ;
	if (st == UGLY_OK){
		st = (name != NULL ? OUT(ugly_set_member(ctx, ((proxy *)self)->h, name, v))
			: OUT(ugly_set_index(ctx, ((proxy *)self)->h, (int64_t)index, v))) ;
		ugly_val_release(ctx, &v) ;
	}
	if (st != UGLY_OK){
		raise_ugly(ctx, st) ;
		return -1 ;
	}
	return 0 ;
}


static Py_ssize_t proxy_length(PyObject *self){
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return -1 ;
	}
	int64_t n = 0 ;
	ugly_status st = OUT(ugly_length(ctx, ((proxy *)self)->h, &n)) ;
	if (st == UGLY_ERR_NOT_CAPABLE){
		/* Python's own error for len() of what has none: list(p) and others skip it (a length hint). */
		PyErr_SetString(PyExc_TypeError, "ugly: the foreign value has no length") ;
		return -1 ;
	}
	if (st != UGLY_OK){
		raise_ugly(ctx, st) ;
		return -1 ;
	}
	return (Py_ssize_t)n ;
}


static PyObject *proxy_richcompare(PyObject *a, PyObject *b, int op){
	if (((op != Py_EQ)&&(op != Py_NE))||(! PyObject_TypeCheck(b, &ProxyType))){
		Py_RETURN_NOTIMPLEMENTED ;
	}
	bool same = false ;
	ugly_ctx *ctx = need_ctx() ;
	if (ctx != NULL){
		ugly_same(ctx, ((proxy *)a)->h, ((proxy *)b)->h, &same) ;
	}
	return PyBool_FromLong((op == Py_EQ) == same) ;
}


static Py_hash_t proxy_hash(PyObject *self){
	Py_hash_t h = (Py_hash_t)((proxy *)self)->h ;
	return (h == -1 ? -2 : h) ;
}


static PyObject *proxy_repr(PyObject *self){
	ugly_ctx *ctx = ugly_current_ctx() ;
	uint32_t traits = 0 ;
	ugly_val tn = ugly_null() ;
	PyObject *r ;
	if ((ctx != NULL)&&(OUT(ugly_describe(ctx, ((proxy *)self)->h, &traits, &tn)) == UGLY_OK)){
		r = PyUnicode_FromFormat("<ugly.Proxy %s>", tn.as.str.ptr) ;
	}
	else {
		r = PyUnicode_FromString("<ugly.Proxy (stale)>") ;
	}
	ugly_val_release(ctx, &tn) ;
	return r ;
}


/*
 * iter(p): the value's own iteration (ugly_iter), as a proxy. A value its
 * language doesn't iterate but that has elements gets what Python gives any
 * object with __getitem__ and no __iter__: the sequence protocol (a Lua
 * table's elements, 0-based through proxy_item).
 */
static PyObject *proxy_iter(PyObject *self){
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return NULL ;
	}
	ugly_handle it = UGLY_NO_HANDLE ;
	ugly_status st = OUT(ugly_iter(ctx, ((proxy *)self)->h, &it)) ;
	if ((st == UGLY_ERR_NOT_CAPABLE)||(st == UGLY_ERR_NOT_IMPLEMENTED)){
		if (proxy_traits(ctx, (proxy *)self) & UGLY_TRAIT_ELEMENTS){
			return PySeqIter_New(self) ;
		}
		PyErr_SetString(PyExc_TypeError, "ugly: the foreign value is not iterable") ;
		return NULL ;
	}
	ugly_val v = ugly_hval(it) ;
	return result(ctx, st, &v) ;
}


/* next(p), on a proxy of a foreign iterator: one value per step (a loop never runs it ahead). */
static PyObject *proxy_iternext(PyObject *self){
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return NULL ;
	}
	ugly_val v = ugly_null() ;
	size_t got = 0 ;
	ugly_status st = OUT(ugly_next(ctx, ((proxy *)self)->h, UGLY_ANY, &v, 1, &got)) ;
	if ((st == UGLY_ERR_NOT_CAPABLE)||(st == UGLY_ERR_NOT_IMPLEMENTED)){
		PyErr_SetString(PyExc_TypeError, "ugly: the foreign value is not an iterator") ;
		return NULL ;
	}
	if ((st == UGLY_OK)&&(got == 0)){
		return NULL ;     /* the end: StopIteration */
	}
	return result(ctx, st, &v) ;
}


/* str(p): the value as its language makes it a string (ugly_to_string); repr() where it has none. */
static PyObject *proxy_str(PyObject *self){
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return NULL ;
	}
	ugly_val v = ugly_null() ;
	ugly_status st = OUT(ugly_to_string(ctx, ((proxy *)self)->h, &v)) ;
	if ((st == UGLY_ERR_NOT_SUPPORTED)||(st == UGLY_ERR_NOT_IMPLEMENTED)){
		return proxy_repr(self) ;
	}
	if ((st == UGLY_OK)&&(v.kind == UGLY_BYTES)){
		/* A language whose strings are bytes (Lua, Perl): text, with what isn't UTF-8 replaced. */
		PyObject *r = PyUnicode_DecodeUTF8((const char *)v.as.bytes.ptr, (Py_ssize_t)v.as.bytes.len, "replace") ;
		ugly_val_release(ctx, &v) ;
		return r ;
	}
	return result(ctx, st, &v) ;
}


static void proxy_dealloc(PyObject *self){
	proxy *p = (proxy *)self ;
	PyObject *t, *v, *tb ;
	PyErr_Fetch(&t, &v, &tb) ;
	if (p->weakrefs != NULL){
		PyObject_ClearWeakRefs(self) ;
	}
	if ((proxy_cache != NULL)&&(p->h != UGLY_NO_HANDLE)){
		PyObject *key = PyLong_FromUnsignedLongLong(p->h) ;
		PyObject *ref = (key != NULL ? PyDict_GetItemWithError(proxy_cache, key) : NULL) ;
		if ((ref != NULL)&&(PyWeakref_GetObject(ref) == Py_None)){
			PyDict_DelItem(proxy_cache, key) ;
		}
		Py_XDECREF(key) ;
		ugly_release(ugly_current_ctx(), p->h) ;
	}
	PyErr_Restore(t, v, tb) ;
	PyObject_Del(self) ;
}


static PyMappingMethods proxy_mapping = {
	.mp_length = proxy_length,
	.mp_subscript = proxy_subscript,
	.mp_ass_subscript = proxy_ass_subscript,
} ;

static PySequenceMethods proxy_sequence = {
	.sq_length = proxy_length,
	.sq_item = proxy_item,
} ;

static PyTypeObject ProxyType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "ugly.Proxy",
	.tp_doc = "A value that lives in another language's runtime.",
	.tp_basicsize = sizeof(proxy),
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_dealloc = proxy_dealloc,
	.tp_getattro = proxy_getattro,
	.tp_setattro = proxy_setattro,
	.tp_call = proxy_call,
	.tp_as_mapping = &proxy_mapping,
	.tp_as_sequence = &proxy_sequence,
	.tp_richcompare = proxy_richcompare,
	.tp_hash = proxy_hash,
	.tp_repr = proxy_repr,
	.tp_str = proxy_str,
	.tp_iter = proxy_iter,
	.tp_iternext = proxy_iternext,
	.tp_weaklistoffset = offsetof(proxy, weakrefs),
} ;


/* ---------------------------------------------------------------- */
/* The embedded "ugly" module                                       */
/* ---------------------------------------------------------------- */

/* ugly.rethrow(e): raise a foreign exception object (a proxy) as itself. */
static PyObject *mod_rethrow(PyObject *mod, PyObject *e){
	(void)mod ;
	if (PyExceptionInstance_Check(e)){
		PyErr_SetObject((PyObject *)Py_TYPE(e), e) ;
		return NULL ;
	}
	PyObject *exc = PyObject_CallFunction(ErrorType, "s", "rethrown foreign exception") ;
	if (exc == NULL){
		return NULL ;
	}
	PyObject_SetAttrString(exc, "payload", e) ;
	PyErr_SetObject(ErrorType, exc) ;
	Py_DECREF(exc) ;
	return NULL ;
}

/* ugly.module(lang, name): a module of any language; Python's own come from import. */
static PyObject *mod_module(PyObject *mod, PyObject *args){
	(void)mod ;
	const char *lang, *name ;
	if (! PyArg_ParseTuple(args, "ss:module", &lang, &name)){
		return NULL ;
	}
	if (strcmp(lang, "python") == 0){
		return PyImport_ImportModule(name) ;
	}
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return NULL ;
	}
	ugly_rt *rt ;
	ugly_status st = OUT(ugly_rt_get(ctx, lang, &rt)) ;
	if (st != UGLY_OK){
		return raise_ugly(ctx, st) ;
	}
	ugly_handle h ;
	st = OUT(ugly_rt_import(ctx, rt, name, &h)) ;
	ugly_val v = ugly_hval(h) ;
	return result(ctx, st, &v) ;
}


/* ugly.eval(lang, code): the value of code in any language. */
static PyObject *mod_eval(PyObject *mod, PyObject *args){
	(void)mod ;
	const char *lang, *code ;
	Py_ssize_t len ;
	if (! PyArg_ParseTuple(args, "ss#:eval", &lang, &code, &len)){
		return NULL ;
	}
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return NULL ;
	}
	ugly_rt *rt ;
	ugly_status st = OUT(ugly_rt_get(ctx, lang, &rt)) ;
	if (st != UGLY_OK){
		return raise_ugly(ctx, st) ;
	}
	ugly_val out = ugly_null() ;
	st = OUT(ugly_rt_eval(ctx, rt, code, (size_t)len, UGLY_ANY, &out)) ;
	return result(ctx, st, &out) ;
}


/* ugly_configure for each value of one option: a str, or a list or tuple of them. */
static int configure_one(ugly_ctx *ctx, const char *lang, const char *key, PyObject *v){
	if (PyUnicode_Check(v)){
		ugly_status st = ugly_configure(ctx, lang, key, PyUnicode_AsUTF8(v)) ;
		if (st != UGLY_OK){
			raise_ugly(ctx, st) ;
			return -1 ;
		}
		return 0 ;
	}
	if (PyList_Check(v)||PyTuple_Check(v)){
		Py_ssize_t i, n = PySequence_Size(v) ;
		for (i = 0 ; i < n ; i++){
			PyObject *item = PySequence_GetItem(v, i) ;
			int rc = ((item != NULL)&&PyUnicode_Check(item) ? configure_one(ctx, lang, key, item) : -1) ;
			if ((rc != 0)&&(! PyErr_Occurred())){
				PyErr_Format(PyExc_TypeError, "configure: %s takes strings", key) ;
			}
			Py_XDECREF(item) ;
			if (rc != 0){
				return -1 ;
			}
		}
		return 0 ;
	}
	PyErr_Format(PyExc_TypeError, "configure: %s takes a string or a list of strings", key) ;
	return -1 ;
}


/* configure(lang, path=..., classpath=..., options=...): before lang's first use. */
static PyObject *mod_configure(PyObject *mod, PyObject *args, PyObject *kwargs){
	(void)mod ;
	const char *lang ;
	if (! PyArg_ParseTuple(args, "s:configure", &lang)){
		return NULL ;
	}
	ugly_ctx *ctx = need_ctx() ;
	if (ctx == NULL){
		return NULL ;
	}
	PyObject *key, *value ;
	Py_ssize_t pos = 0 ;
	while ((kwargs != NULL)&&PyDict_Next(kwargs, &pos, &key, &value)){
		if (configure_one(ctx, lang, PyUnicode_AsUTF8(key), value) != 0){
			return NULL ;
		}
	}
	Py_RETURN_NONE ;
}


static PyMethodDef mod_methods[] = {
	{ "configure", (PyCFunction)(void (*)(void))mod_configure, METH_VARARGS | METH_KEYWORDS,
		"configure(lang, path=..., ...): options for lang, before its first use." },
	{ "module", mod_module, METH_VARARGS, "module(lang, name): a module of any language." },
	{ "eval", mod_eval, METH_VARARGS, "eval(lang, code): the value of code in any language." },
	{ "rethrow", mod_rethrow, METH_O, "Raise e again; a foreign exception object keeps its identity." },
	{ "opts", (PyCFunction)(void (*)(void))mod_opts, METH_VARARGS | METH_KEYWORDS,
		"opts(hint=None, context=None): call options, for the last argument of a foreign call." },
	{ NULL, NULL, 0, NULL }
} ;

static struct PyModuleDef mod_def = {
	PyModuleDef_HEAD_INIT, "ugly", "UGLY: values and errors from other languages.", -1, mod_methods,
	NULL, NULL, NULL, NULL
} ;


/* ---------------------------------------------------------------- */
/* Operations                                                       */
/* ---------------------------------------------------------------- */

/* Finishes an operation that produced 'r' (new ref, or NULL with an exception). */
static ugly_status out_val(ugly_ctx *ctx, PyObject *r, ugly_kind want, ugly_val *out){
	if (r == NULL){
		return py_failure(ctx) ;
	}
	ugly_status st = to_val(ctx, r, want, out) ;
	Py_DECREF(r) ;
	return st ;
}


static ugly_status py_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out){
	(void)rt ;
	ENTER ;
	ugly_status st = out_val(ctx, PyImport_ImportModule(name), UGLY_HANDLE, out) ;
	LEAVE ;
	return st ;
}


static ugly_status py_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_val *out){
	pyrt *r = ugly_rt_native(rt) ;
	ENTER ;
	Py_INCREF(r->globals) ;
	ugly_status st = out_val(ctx, r->globals, UGLY_HANDLE, out) ;
	LEAVE ;
	return st ;
}


static ugly_status py_eval(ugly_ctx *ctx, ugly_rt *rt, const char *code, size_t len, ugly_kind want, ugly_val *out){
	pyrt *r = ugly_rt_native(rt) ;
	char *src = malloc(len + 1) ;
	if (src == NULL){
		return UGLY_ERR_NOMEM ;
	}
	memcpy(src, code, len) ;
	src[len] = '\0' ;
	ENTER ;
	/* An expression gives its value; statements give None. */
	PyObject *c = Py_CompileString(src, "<eval>", Py_eval_input) ;
	if ((c == NULL)&&(PyErr_ExceptionMatches(PyExc_SyntaxError))){
		PyErr_Clear() ;
		c = Py_CompileString(src, "<eval>", Py_file_input) ;
	}
	PyObject *v = NULL ;
	if (c != NULL){
		v = PyEval_EvalCode(c, r->globals, r->globals) ;
		Py_DECREF(c) ;
	}
	ugly_status st = out_val(ctx, v, want, out) ;
	LEAVE ;
	free(src) ;
	return st ;
}


/* Whether iter() takes o, by the test it applies: __iter__, or the sequence protocol (not a dict's). */
static int iterable(PyObject *o){
	return (Py_TYPE(o)->tp_iter != NULL)||PySequence_Check(o) ;
}


static ugly_status py_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name){
	(void)rt ;
	PyObject *o = ref ;
	ENTER ;
	uint32_t t = 0 ;
	if (PyDict_Check(o)){
		t |= UGLY_TRAIT_MEMBERS ;
	}
	else {
		t |= UGLY_TRAIT_MEMBERS ;     /* attributes */
		if (PySequence_Check(o)&&(! PyUnicode_Check(o))&&(! PyBytes_Check(o))){
			t |= UGLY_TRAIT_ELEMENTS ;
		}
	}
	if (iterable(o)){
		t |= UGLY_TRAIT_ITERABLE ;
	}
	if (PyCallable_Check(o)){
		t |= UGLY_TRAIT_CALLABLE ;
	}
	if (PyType_Check(o)){
		t |= UGLY_TRAIT_INSTANTIABLE ;
	}
	*traits = t ;
	const char *n = Py_TYPE(o)->tp_name ;
	const char *dot = strrchr(n, '.') ;
	n = (dot != NULL ? dot + 1 : n) ;
	ugly_status st = ugly_val_set_string(ctx, type_name, n, strlen(n)) ;
	LEAVE ;
	return st ;
}


static ugly_status py_get_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want, ugly_val *out){
	(void)rt ;
	PyObject *o = ref ;
	ugly_status st ;
	ENTER ;
	if (PyDict_Check(o)){
		PyObject *v = PyDict_GetItemString(o, name) ;     /* borrowed */
		if (v == NULL){
			st = fail(ctx, UGLY_ERR_NOT_FOUND, "KeyError", name) ;
		}
		else {
			Py_INCREF(v) ;
			st = out_val(ctx, v, want, out) ;
		}
	}
	else {
		PyObject *v = PyObject_GetAttrString(o, name) ;
		if ((v == NULL)&&(PyErr_ExceptionMatches(PyExc_AttributeError))){
			st = fail(ctx, UGLY_ERR_NOT_FOUND, "AttributeError", name) ;
		}
		else {
			st = out_val(ctx, v, want, out) ;
		}
	}
	LEAVE ;
	return st ;
}


static ugly_status py_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	(void)rt ;
	PyObject *o = ref ;
	ugly_status st = UGLY_OK ;
	ENTER ;
	PyObject *pv = from_val(ctx, v) ;
	int rc = -1 ;
	if (pv != NULL){
		rc = (PyDict_Check(o) ? PyDict_SetItemString(o, name, pv) : PyObject_SetAttrString(o, name, pv)) ;
		Py_DECREF(pv) ;
	}
	if (rc != 0){
		st = py_failure(ctx) ;
	}
	LEAVE ;
	return st ;
}


/* del d[k] or del o.name; a member that isn't there is not an error. */
static ugly_status py_delete_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name){
	(void)rt ;
	PyObject *o = ref ;
	ugly_status st = UGLY_OK ;
	ENTER ;
	int rc = 0 ;
	if (PyDict_Check(o)){
		PyObject *k = PyUnicode_FromString(name) ;
		rc = ((k == NULL) ? -1 : (PyDict_Contains(o, k) == 1 ? PyDict_DelItem(o, k) : 0)) ;
		Py_XDECREF(k) ;
	}
	else if (PyObject_HasAttrString(o, name)){
		rc = PyObject_DelAttrString(o, name) ;
	}
	if (rc != 0){
		st = py_failure(ctx) ;
	}
	LEAVE ;
	return st ;
}


static ugly_status py_member_names(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	(void)rt ;
	PyObject *o = ref ;
	ugly_status st ;
	ENTER ;
	/* A dict's keys; otherwise dir(), without the dunder names. */
	PyObject *names = (PyDict_Check(o) ? PyDict_Keys(o) : PyObject_Dir(o)) ;
	if (names == NULL){
		st = py_failure(ctx) ;
	}
	else {
		Py_ssize_t n = PyList_GET_SIZE(names), i, k = 0 ;
		ugly_val *v = calloc((size_t)n + 1, sizeof(ugly_val)) ;
		for (i = 0 ; i < n ; i++){
			PyObject *s = PyList_GET_ITEM(names, i) ;
			Py_ssize_t len ;
			const char *u = (PyUnicode_Check(s) ? PyUnicode_AsUTF8AndSize(s, &len) : NULL) ;
			if ((u == NULL)||((! PyDict_Check(o))&&(u[0] == '_')&&(u[1] == '_'))){
				PyErr_Clear() ;
				continue ;
			}
			v[k++] = ugly_str(u, (size_t)len) ;
		}
		st = ugly_list_new(ctx, v, (size_t)k, out) ;
		free(v) ;
		Py_DECREF(names) ;
	}
	LEAVE ;
	return st ;
}


static ugly_status py_length(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out){
	(void)rt ;
	ugly_status st = UGLY_OK ;
	ENTER ;
	Py_ssize_t n = PyObject_Length(ref) ;
	if (n < 0){
		st = (PyErr_ExceptionMatches(PyExc_TypeError) ? fail(ctx, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no length")
			: py_failure(ctx)) ;
	}
	else {
		*out = (int64_t)n ;
	}
	LEAVE ;
	return st ;
}


static ugly_status py_get_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want, ugly_val *out){
	(void)rt ;
	PyObject *o = ref ;
	ugly_status st ;
	ENTER ;
	if ((! PySequence_Check(o))||PyDict_Check(o)){
		st = fail(ctx, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements") ;
	}
	else {
		Py_ssize_t n = PySequence_Size(o) ;
		if ((n >= 0)&&((i < 0)||(i >= n))){
			st = fail(ctx, UGLY_ERR_NOT_FOUND, "IndexError", "index out of range") ;
		}
		else {
			st = out_val(ctx, (n < 0 ? NULL : PySequence_GetItem(o, (Py_ssize_t)i)), want, out) ;
		}
	}
	LEAVE ;
	return st ;
}


static ugly_status py_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	(void)rt ;
	PyObject *o = ref ;
	ugly_status st = UGLY_OK ;
	ENTER ;
	Py_ssize_t n = ((PySequence_Check(o)&&(! PyDict_Check(o))) ? PySequence_Size(o) : -1) ;
	if (n < 0){
		st = fail(ctx, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements") ;
	}
	else if ((i < 0)||(i > n)||((i == n)&&(! PyList_Check(o)))){
		st = fail(ctx, UGLY_ERR_NOT_FOUND, "IndexError", "index out of range") ;
	}
	else {
		PyObject *pv = from_val(ctx, v) ;
		int rc = -1 ;
		if (pv != NULL){
			rc = (i == n ? PyList_Append(o, pv) : PySequence_SetItem(o, (Py_ssize_t)i, pv)) ;     /* one past the end appends */
			Py_DECREF(pv) ;
		}
		if (rc != 0){
			st = py_failure(ctx) ;
		}
	}
	LEAVE ;
	return st ;
}


/* seq[offset:offset + remove] = insert, on any mutable sequence. */
static ugly_status py_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n){

	(void)rt ;
	PyObject *o = ref ;
	ugly_status st = UGLY_OK ;
	ENTER ;
	if ((! PySequence_Check(o))||PyDict_Check(o)){
		st = fail(ctx, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements") ;
	}
	else if (! PyObject_HasAttrString(o, "__setitem__")){
		st = fail(ctx, UGLY_ERR_NOT_CAPABLE, "TypeError", "the sequence is immutable") ;
	}
	else {
		PyObject *l = PyList_New((Py_ssize_t)n) ;
		size_t i ;
		for (i = 0 ; (l != NULL)&&(i < n) ; i++){
			PyObject *pv = from_val(ctx, insert[i]) ;
			if (pv == NULL){
				Py_CLEAR(l) ;
				break ;
			}
			PyList_SET_ITEM(l, (Py_ssize_t)i, pv) ;
		}
		if ((l == NULL)||(PySequence_SetSlice(o, (Py_ssize_t)offset, (Py_ssize_t)(offset + remove), l) != 0)){
			st = py_failure(ctx) ;
		}
		Py_XDECREF(l) ;
	}
	LEAVE ;
	return st ;
}


/*
 * Call options (contract §11): keyword arguments as a dict (NULL for none);
 * a hint or a call context is refused, before anything else happens. With
 * the GIL held.
 */
static ugly_status kwargs_dict(ugly_ctx *ctx, const ugly_opts *o, PyObject **out){
	*out = NULL ;
	if (o == NULL){
		return UGLY_OK ;
	}
	if (o->hint != NULL){
		return fail(ctx, UGLY_ERR_NOT_SUPPORTED, "TypeError", "Python takes no call hint") ;
	}
	if (o->context != NULL){
		return fail(ctx, UGLY_ERR_NOT_SUPPORTED, "TypeError", "Python takes no call context") ;
	}
	if (o->nkwargs == 0){
		return UGLY_OK ;
	}
	PyObject *d = PyDict_New() ;
	size_t i ;
	for (i = 0 ; (d != NULL)&&(i < o->nkwargs) ; i++){
		PyObject *v = from_val(ctx, o->kwargs[i].value) ;
		if ((v == NULL)||(PyDict_SetItemString(d, o->kwargs[i].name, v) != 0)){
			Py_XDECREF(v) ;
			Py_CLEAR(d) ;
			break ;
		}
		Py_DECREF(v) ;
	}
	if (d == NULL){
		return py_failure(ctx) ;
	}
	*out = d ;
	return UGLY_OK ;
}


static ugly_status call_with(ugly_ctx *ctx, void *ref, const ugly_val *args, size_t nargs, const ugly_opts *o,
	ugly_kind want, ugly_val *out){

	ugly_status st ;
	ENTER ;
	PyObject *kw = NULL ;
	if ((st = kwargs_dict(ctx, o, &kw)) != UGLY_OK){
		/* refused */
	}
	else if (! PyCallable_Check(ref)){
		st = fail(ctx, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not callable") ;
	}
	else {
		PyObject *t = args_tuple(ctx, args, nargs) ;
		PyObject *r = (t != NULL ? PyObject_Call(ref, t, kw) : NULL) ;
		Py_XDECREF(t) ;
		st = out_val(ctx, r, want, out) ;
	}
	Py_XDECREF(kw) ;
	LEAVE ;
	return st ;
}


static ugly_status py_call(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out){

	(void)rt ;
	return call_with(ctx, ref, args, nargs, NULL, want, out) ;
}


static ugly_status py_call_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_kind want, ugly_val *out){

	(void)rt ;
	return call_with(ctx, ref, args, nargs, o, want, out) ;
}


static ugly_status invoke_with(ugly_ctx *ctx, void *ref, const char *method, const ugly_val *args,
	size_t nargs, const ugly_opts *o, ugly_kind want, ugly_val *out){

	ugly_status st ;
	ENTER ;
	PyObject *kw = NULL ;
	if ((st = kwargs_dict(ctx, o, &kw)) != UGLY_OK){
		LEAVE ;
		return st ;
	}
	PyObject *m = PyObject_GetAttrString(ref, method) ;     /* a bound method: self is already in it */
	if ((m == NULL)&&(PyErr_ExceptionMatches(PyExc_AttributeError))){
		st = fail(ctx, UGLY_ERR_NOT_FOUND, "AttributeError", method) ;
	}
	else if (m == NULL){
		st = py_failure(ctx) ;
	}
	else {
		PyObject *t = args_tuple(ctx, args, nargs) ;
		PyObject *r = (t != NULL ? PyObject_Call(m, t, kw) : NULL) ;
		Py_XDECREF(t) ;
		Py_DECREF(m) ;
		st = out_val(ctx, r, want, out) ;
	}
	Py_XDECREF(kw) ;
	LEAVE ;
	return st ;
}


static ugly_status py_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out){

	(void)rt ;
	return invoke_with(ctx, ref, method, args, nargs, NULL, want, out) ;
}


static ugly_status py_invoke_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, const ugly_opts *o, ugly_kind want, ugly_val *out){

	(void)rt ;
	return invoke_with(ctx, ref, method, args, nargs, o, want, out) ;
}


static ugly_status new_with(ugly_ctx *ctx, void *ref, const ugly_val *args, size_t nargs, const ugly_opts *o,
	ugly_handle *out){

	ugly_val v = ugly_null() ;
	ugly_status st ;
	ENTER ;
	if (! PyType_Check((PyObject *)ref)){
		st = fail(ctx, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not a class") ;
	}
	else {
		st = call_with(ctx, ref, args, nargs, o, UGLY_HANDLE, &v) ;
		if (st == UGLY_OK){
			*out = v.as.h ;
		}
	}
	LEAVE ;
	return st ;
}


static ugly_status py_new_instance(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_handle *out){

	(void)rt ;
	return new_with(ctx, ref, args, nargs, NULL, out) ;
}


static ugly_status py_new_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_handle *out){

	(void)rt ;
	return new_with(ctx, ref, args, nargs, o, out) ;
}


/* iter(o); on an iterator, by Python's convention, itself. */
static ugly_status py_iter(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	(void)rt ;
	PyObject *o = ref ;
	ugly_status st ;
	ENTER ;
	if (! iterable(o)){
		st = fail(ctx, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not iterable") ;
	}
	else {
		PyObject *it = PyObject_GetIter(o) ;     /* __iter__ may raise: that's RAISED */
		if (it == NULL){
			st = py_failure(ctx) ;
		}
		else {
			st = export_obj(ctx, it, out) ;
			Py_DECREF(it) ;
		}
	}
	LEAVE ;
	return st ;
}


/* next(it), up to max times: a generator runs only as far as asked. Values made before an exception are kept. */
static ugly_status py_next(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_kind want, ugly_val *out, size_t max, size_t *got){
	(void)rt ;
	PyObject *o = ref ;
	ugly_status st = UGLY_OK ;
	size_t n = 0 ;
	ENTER ;
	if (! PyIter_Check(o)){
		st = fail(ctx, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not an iterator") ;
	}
	while ((st == UGLY_OK)&&(n < max)){
		PyObject *v = PyIter_Next(o) ;
		if (v == NULL){
			if (PyErr_Occurred()){
				st = py_failure(ctx) ;
			}
			break ;     /* StopIteration: the end */
		}
		st = to_val(ctx, v, want, &out[n]) ;
		Py_DECREF(v) ;
		if (st == UGLY_OK){
			n++ ;
		}
	}
	*got = n ;
	LEAVE ;
	return st ;
}


/* str(o). */
static ugly_status py_to_string(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_val *out){
	(void)rt ;
	ENTER ;
	ugly_status st = out_val(ctx, PyObject_Str((PyObject *)ref), UGLY_STRING, out) ;
	LEAVE ;
	return st ;
}


static void py_release(ugly_rt *rt, void *ref){
	(void)rt ;
	ENTER ;
	Py_DECREF((PyObject *)ref) ;
	LEAVE ;
}


/* ---------------------------------------------------------------- */
/* Lifecycle                                                        */
/* ---------------------------------------------------------------- */

/*
 * Fork. A program that forks after Python started (HAProxy's master starting
 * its workers, with Python used at load time) copies only the forking
 * thread, and CPython must be told: os.fork() calls PyOS_BeforeFork and
 * PyOS_AfterFork_Parent/Child around fork(), a C program doesn't. Without
 * them a child inherits the parent's thread states and locks, and
 * deadlocks if another thread held the GIL. These pthread_atfork handlers
 * bracket any fork the same way: the GIL taken (so no other thread holds
 * it), then PyOS_BeforeFork; after it, PyOS_AfterFork_*, then the GIL let go.
 * os.fork() itself is left to Python: its before-callback marks this thread.
 */
static _Thread_local int python_forking ;     /* os.fork() in progress on this thread */
static _Thread_local int fork_bracketed ;
static _Thread_local PyGILState_STATE fork_gil ;

static void at_fork_prepare(void){
	if (python_forking || (! Py_IsInitialized())){
		return ;
	}
	fork_gil = PyGILState_Ensure() ;
	PyOS_BeforeFork() ;
	fork_bracketed = 1 ;
}


static void at_fork_parent(void){
	if (fork_bracketed){
		fork_bracketed = 0 ;
		PyOS_AfterFork_Parent() ;
		PyGILState_Release(fork_gil) ;
	}
}


static void at_fork_child(void){
	if (fork_bracketed){
		fork_bracketed = 0 ;
		PyOS_AfterFork_Child() ;
		PyGILState_Release(fork_gil) ;
	}
}


static PyObject *fork_mark(PyObject *self, PyObject *unused){
	(void)self ; (void)unused ;
	python_forking = 1 ;
	Py_RETURN_NONE ;
}


static PyObject *fork_unmark(PyObject *self, PyObject *unused){
	(void)self ; (void)unused ;
	python_forking = 0 ;
	Py_RETURN_NONE ;
}


static PyMethodDef fork_defs[] = {
	{ "_fork_mark", fork_mark, METH_NOARGS, NULL },
	{ "_fork_unmark", fork_unmark, METH_NOARGS, NULL },
} ;


/* Once per process, with the GIL held. */
static void fork_safety(void){
	static int done = 0 ;
	if (done){
		return ;
	}
	done = 1 ;
	pthread_atfork(at_fork_prepare, at_fork_parent, at_fork_child) ;
	PyObject *os = PyImport_ImportModule("os") ;
	PyObject *reg = (os != NULL ? PyObject_GetAttrString(os, "register_at_fork") : NULL) ;
	PyObject *mark = PyCFunction_New(&fork_defs[0], NULL), *unmark = PyCFunction_New(&fork_defs[1], NULL) ;
	PyObject *kw = Py_BuildValue("{sOsOsO}", "before", mark, "after_in_parent", unmark, "after_in_child", unmark) ;
	PyObject *empty = PyTuple_New(0) ;
	PyObject *r = ((reg != NULL)&&(kw != NULL)&&(empty != NULL) ? PyObject_Call(reg, empty, kw) : NULL) ;
	if (r == NULL){
		PyErr_Clear() ;
	}
	Py_XDECREF(r) ;
	Py_XDECREF(empty) ;
	Py_XDECREF(kw) ;
	Py_XDECREF(mark) ;
	Py_XDECREF(unmark) ;
	Py_XDECREF(reg) ;
	Py_XDECREF(os) ;
}


static ugly_status py_init(ugly_ctx *ctx, const char *dir){
	(void)ctx ; (void)dir ;
	if (! Py_IsInitialized()){
		/* We start Python: no signal handlers, and the GIL released for PyGILState_Ensure. */
		Py_InitializeEx(0) ;
		PyEval_SaveThread() ;
	}
	ENTER ;
	fork_safety() ;
	ugly_status st = UGLY_OK ;
	if (ugly_module == NULL){
		if ((PyType_Ready(&ProxyType) < 0)
			||((ugly_module = PyModule_Create(&mod_def)) == NULL)
			||((ErrorType = PyErr_NewExceptionWithDoc("ugly.Error",
				"An error from another language: .type, .message, .language, .payload", NULL, NULL)) == NULL)
			||((proxy_cache = PyDict_New()) == NULL)){
			PyErr_Clear() ;
			st = UGLY_ERR_INTERNAL ;
		}
		else {
			Py_INCREF(&ProxyType) ;
			PyModule_AddObject(ugly_module, "Proxy", (PyObject *)&ProxyType) ;
			Py_INCREF(ErrorType) ;
			PyModule_AddObject(ugly_module, "Error", ErrorType) ;
			OptsType = PyObject_CallFunction((PyObject *)&PyType_Type, "s(){}", "Opts") ;
			if (OptsType != NULL){
				Py_INCREF(OptsType) ;
				PyModule_AddObject(ugly_module, "Opts", OptsType) ;
			}
			PyDict_SetItemString(PyImport_GetModuleDict(), "ugly", ugly_module) ;
		}
	}
	if (st == UGLY_OK){
		/* ugly_configure("python", "path", dir): ahead of PYTHONPATH, in the order given. */
		PyObject *sys_path = PySys_GetObject("path") ;     /* borrowed */
		size_t n = 0 ;
		while (ugly_option("python", "path", n) != NULL){
			n++ ;
		}
		while ((sys_path != NULL)&&(n-- > 0)){
			PyObject *d = PyUnicode_DecodeFSDefault(ugly_option("python", "path", n)) ;
			if ((d == NULL)||(PyList_Insert(sys_path, 0, d) != 0)){
				PyErr_Clear() ;
				st = UGLY_ERR_INTERNAL ;
			}
			Py_XDECREF(d) ;
		}
	}
	LEAVE ;
	return st ;
}


static ugly_status py_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	(void)ctx ;
	pyrt *r = calloc(1, sizeof(pyrt)) ;
	if (r == NULL){
		return UGLY_ERR_NOMEM ;
	}
	r->urt = rt ;
	ENTER ;
	r->globals = PyDict_New() ;
	PyDict_SetItemString(r->globals, "__builtins__", PyEval_GetBuiltins()) ;
	PyObject *name = PyUnicode_FromString("__ugly__") ;
	PyDict_SetItemString(r->globals, "__name__", name) ;
	Py_XDECREF(name) ;
	LEAVE ;
	current_rt = r ;
	*native = r ;
	return UGLY_OK ;
}


/* The host's own Python, when Python is the host (the binding): its __main__ is the namespace. */
static ugly_status py_rt_attach(ugly_ctx *ctx, ugly_rt *rt, void *existing, void **native){
	(void)ctx ; (void)existing ;
	pyrt *r = calloc(1, sizeof(pyrt)) ;
	if (r == NULL){
		return UGLY_ERR_NOMEM ;
	}
	r->urt = rt ;
	ENTER ;
	PyObject *main = PyImport_AddModule("__main__") ;     /* borrowed */
	r->globals = (main != NULL ? PyModule_GetDict(main) : NULL) ;
	Py_XINCREF(r->globals) ;
	PyErr_Clear() ;
	LEAVE ;
	if (r->globals == NULL){
		free(r) ;
		return UGLY_ERR_INTERNAL ;
	}
	current_rt = r ;
	*native = r ;
	return UGLY_OK ;
}


static ugly_status py_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	(void)ctx ; (void)rt ;
	pyrt *r = native ;
	ENTER ;
	Py_CLEAR(r->globals) ;
	PyGC_Collect() ;          /* cycles holding proxies release their handles now */
	LEAVE ;
	if (current_rt == r){
		current_rt = NULL ;
	}
	free(r) ;
	return UGLY_OK ;
}


static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = UGLY_CAP_EVAL | UGLY_CAP_ATTACH | UGLY_CAP_ANY_THREAD,     /* the GIL serializes every entry */
	.name = "python",
	.init = py_init,
	.rt_new = py_rt_new,
	.options = (const char *const []){ "path", NULL },
	.rt_attach = py_rt_attach,
	.rt_delete = py_rt_delete,
	.import = py_import,
	.globals = py_globals,
	.eval = py_eval,
	.describe = py_describe,
	.get_member = py_get_member,
	.set_member = py_set_member,
	.member_names = py_member_names,
	.length = py_length,
	.get_index = py_get_index,
	.set_index = py_set_index,
	.call_opts = py_call_opts,
	.invoke_opts = py_invoke_opts,
	.new_opts = py_new_opts,
	.delete_member = py_delete_member,
	.splice = py_splice,
	.call = py_call,
	.invoke = py_invoke,
	.new_instance = py_new_instance,
	.iter = py_iter,
	.next = py_next,
	.to_string = py_to_string,
	.release = py_release,
} ;


const ugly_plugin *ugly_plugin_python(void){
	return &plugin ;
}


#ifdef UGLY_BINDING
/* ---------------------------------------------------------------- */
/* The binding: import ugly from a plain python3 process            */
/* ---------------------------------------------------------------- */

/*
 * Built from this same file as the "ugly" extension module, without
 * libpython: its Python symbols come from the host interpreter. It registers
 * this plugin as "python" (loading ugly_python.so would bring a second
 * CPython into the process), attaches to the host's interpreter, and returns
 * the same ugly module embedded runtimes get. An atexit handler tears down
 * everything it created.
 */
static ugly_ctx *b_ctx ;
static ugly_lang *b_lang ;
static ugly_rt *b_rt ;
static int b_owns_ctx ;

static PyObject *binding_teardown(PyObject *mod, PyObject *noargs){
	(void)mod ; (void)noargs ;
	if (b_rt != NULL){
		ugly_rt_delete(b_ctx, b_rt) ;
		ugly_lang_unload(b_ctx, b_lang) ;
		if (b_owns_ctx){
			ugly_ctx_delete(b_ctx) ;     /* also deletes the runtimes ugly.module created */
		}
		b_rt = NULL ;
	}
	Py_RETURN_NONE ;
}

static PyMethodDef teardown_def = { "_teardown", binding_teardown, METH_NOARGS, "Detach UGLY (run at exit)." } ;


PyMODINIT_FUNC PyInit_ugly(void){
	if (b_rt == NULL){
		b_ctx = ugly_current_ctx() ;
		if (b_ctx == NULL){
			if (ugly_ctx_new(&b_ctx) != UGLY_OK){
				PyErr_SetString(PyExc_ImportError, "ugly: can't create a context") ;
				return NULL ;
			}
			b_owns_ctx = 1 ;
		}
		ugly_status st = ugly_lang_register(b_ctx, &plugin, &b_lang) ;
		if (st == UGLY_OK){
			st = ugly_rt_attach(b_ctx, b_lang, PyImport_AddModule("__main__"), &b_rt) ;
		}
		if (st != UGLY_OK){
			const ugly_error *e = ugly_last_error(b_ctx) ;
			PyErr_Format(PyExc_ImportError, "ugly: can't attach: %s", e ? ugly_error_message(e) : "?") ;
			return NULL ;
		}
		PyObject *atexit = PyImport_ImportModule("atexit") ;
		PyObject *f = PyCFunction_New(&teardown_def, NULL) ;
		PyObject *r = ((atexit != NULL)&&(f != NULL) ? PyObject_CallMethod(atexit, "register", "O", f) : NULL) ;
		Py_XDECREF(r) ;
		Py_XDECREF(f) ;
		Py_XDECREF(atexit) ;
		if (r == NULL){
			return NULL ;
		}
	}
	Py_INCREF(ugly_module) ;
	return ugly_module ;
}
#endif
