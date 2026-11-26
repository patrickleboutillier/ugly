/*
 * The Java plugin. One JVM per process (the JNI invocation API can't create
 * a second one, or a new one after DestroyJavaVM), so the plugin is neither
 * UGLY_CAP_MULTI_RUNTIME nor UGLY_CAP_RESTARTABLE; a runtime is the JVM as
 * seen from one thread.
 *
 * - The JVM is created at plugin load with -Xrs (no signal handlers of its
 *   own beyond what it needs), and the class path is the helper jar (ugly.jar,
 *   next to this plugin) plus UGLY_JAVA_CLASSPATH and CLASSPATH.
 * - Most of the protocol is Java (ugly.Bridge): reflection, members,
 *   elements, and overload resolution by scoring how well arguments convert.
 *   This file converts values, maps exceptions, and implements the natives
 *   behind ugly.Foreign (RegisterNatives).
 * - Native refs are JNI global refs. Identity comes from Bridge.acquireId
 *   (objects move, so their address can't be it).
 * - Strings cross as UTF-16, converted here (JNI's "modified UTF-8" isn't
 *   UTF-8: it mangles NUL and characters outside the BMP).
 * - Conversions follow Java's rules: boxed integers -> INT, Double/Float ->
 *   DOUBLE (and integers widen to DOUBLE on request), String/Character ->
 *   STRING, byte[] -> BYTES; nothing else converts implicitly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jni.h>

#include "ugly_plugin.h"


static JavaVM *jvm ;

/*
 * The runtime: the JVM, from any thread (UGLY_CAP_ANY_THREAD). Each
 * operation uses its own thread's JNIEnv (env_here), attaching the thread
 * if it's new to the JVM.
 */
typedef struct jrt {
	ugly_rt *urt ;
} jrt ;

static jrt *the_rt ;             /* one runtime at a time */

/* Classes and members, global refs, looked up once. */
static struct {
	jclass Bridge, Foreign, ForeignException, NotFound, NotCapable, TypeMismatch, Throwable, Class, Object ;
	jclass Boolean, Long, Integer, Short, Byte, Double, Float, Character, String, BigInteger, Number, ByteArray ;
	jmethodID b_importClass, b_traits, b_typeName, b_getMember, b_setMember, b_memberNames, b_length,
		b_getIndex, b_setIndex, b_deleteMember, b_splice, b_hint, b_call, b_invoke, b_newInstance, b_acquireId, b_releaseId, b_foreignOf, b_kind ;
	jmethodID f_of ;
	jfieldID f_handle ;
	jmethodID fe_new, fe_take, fe_payload ;
	jmethodID t_getMessage, c_getSimpleName, o_getClass ;
	jmethodID bool_valueOf, bool_value, long_valueOf, double_valueOf, num_longValue, num_doubleValue ;
	jmethodID char_value, bi_bitLength ;
} J ;


static JNIEnv *env_here(void){
	JNIEnv *env = NULL ;
	if ((*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_10) == JNI_EDETACHED){
		/* As a daemon: a thread that ends without detaching mustn't hold up DestroyJavaVM. */
		(*jvm)->AttachCurrentThreadAsDaemon(jvm, (void **)&env, NULL) ;
	}
	return env ;
}


/* Clears a pending exception we don't expect to matter (cleanup paths); 1 if there was one. */
static int drop_exception(JNIEnv *env){
	if ((*env)->ExceptionCheck(env)){
		(*env)->ExceptionClear(env) ;
		return 1 ;
	}
	return 0 ;
}


static ugly_rt *URT(void){
	return (the_rt != NULL ? the_rt->urt : NULL) ;
}


static ugly_status fail(ugly_ctx *ctx, ugly_status st, const char *type, const char *msg){
	return ugly_fail(ctx, URT(), st, type, msg) ;
}


/* ---------------------------------------------------------------- */
/* Strings: UTF-8 <-> UTF-16                                        */
/* ---------------------------------------------------------------- */

/* A Java string from UTF-8 (NULL if it isn't valid UTF-8). */
static jstring new_string(JNIEnv *env, const char *s, size_t len){
	jchar *u = malloc((len + 1) * sizeof(jchar)) ;
	if (u == NULL){
		return NULL ;
	}
	size_t i = 0, n = 0 ;
	while (i < len){
		unsigned char c = (unsigned char)s[i] ;
		uint32_t cp ;
		size_t k ;
		if (c < 0x80){ cp = c ; k = 0 ; }
		else if ((c & 0xE0) == 0xC0){ cp = c & 0x1F ; k = 1 ; }
		else if ((c & 0xF0) == 0xE0){ cp = c & 0x0F ; k = 2 ; }
		else if ((c & 0xF8) == 0xF0){ cp = c & 0x07 ; k = 3 ; }
		else { free(u) ; return NULL ; }
		if ((k > 0)&&(i + k >= len)){ free(u) ; return NULL ; }     /* truncated sequence */
		size_t j ;
		for (j = 1 ; j <= k ; j++){
			if (((unsigned char)s[i + j] & 0xC0) != 0x80){ free(u) ; return NULL ; }
			cp = (cp << 6) | ((unsigned char)s[i + j] & 0x3F) ;
		}
		i += k + 1 ;
		if (cp >= 0x10000){
			cp -= 0x10000 ;
			u[n++] = (jchar)(0xD800 + (cp >> 10)) ;
			u[n++] = (jchar)(0xDC00 + (cp & 0x3FF)) ;
		}
		else {
			u[n++] = (jchar)cp ;
		}
	}
	jstring js = (*env)->NewString(env, u, (jsize)n) ;
	free(u) ;
	return js ;
}


/* A Java string as UTF-8 into an owned result (UGLY_ERR_TYPE on a lone surrogate). */
static ugly_status string_val(JNIEnv *env, ugly_ctx *ctx, jstring js, ugly_val *out){
	jsize n = (*env)->GetStringLength(env, js) ;
	const jchar *u = (*env)->GetStringChars(env, js, NULL) ;
	char *s = malloc((size_t)n * 3 + 1) ;
	size_t len = 0 ;
	ugly_status st = UGLY_OK ;
	jsize i ;
	for (i = 0 ; (i < n)&&(st == UGLY_OK) ; i++){
		uint32_t cp = u[i] ;
		if ((cp >= 0xD800)&&(cp <= 0xDBFF)&&(i + 1 < n)&&(u[i + 1] >= 0xDC00)&&(u[i + 1] <= 0xDFFF)){
			cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00) ;
			i++ ;
		}
		else if ((cp >= 0xD800)&&(cp <= 0xDFFF)){
			st = fail(ctx, UGLY_ERR_TYPE, "TypeError", "string has a lone surrogate") ;
			break ;
		}
		if (cp < 0x80){ s[len++] = (char)cp ; }
		else if (cp < 0x800){ s[len++] = (char)(0xC0 | (cp >> 6)) ; s[len++] = (char)(0x80 | (cp & 0x3F)) ; }
		else if (cp < 0x10000){ s[len++] = (char)(0xE0 | (cp >> 12)) ; s[len++] = (char)(0x80 | ((cp >> 6) & 0x3F)) ; s[len++] = (char)(0x80 | (cp & 0x3F)) ; }
		else { s[len++] = (char)(0xF0 | (cp >> 18)) ; s[len++] = (char)(0x80 | ((cp >> 12) & 0x3F)) ; s[len++] = (char)(0x80 | ((cp >> 6) & 0x3F)) ; s[len++] = (char)(0x80 | (cp & 0x3F)) ; }
	}
	(*env)->ReleaseStringChars(env, js, u) ;
	if (st == UGLY_OK){
		st = ugly_val_set_string(ctx, out, s, len) ;
	}
	free(s) ;
	return st ;
}


/* A C copy of a Java string (UTF-8, for messages and type names). */
static char *c_string(JNIEnv *env, jstring js){
	if (js == NULL){
		return strdup("") ;
	}
	ugly_val v ;
	ugly_ctx *ctx = ugly_current_ctx() ;
	char *out = NULL ;
	if ((ctx != NULL)&&(string_val(env, ctx, js, &v) == UGLY_OK)){
		out = strdup(v.as.str.ptr) ;
		ugly_val_release(ctx, &v) ;
	}
	else {
		const char *m = (*env)->GetStringUTFChars(env, js, NULL) ;
		out = strdup(m != NULL ? m : "") ;
		if (m != NULL){
			(*env)->ReleaseStringUTFChars(env, js, m) ;
		}
	}
	return out ;
}


/* ---------------------------------------------------------------- */
/* Java -> ugly_val                                                 */
/* ---------------------------------------------------------------- */

static ugly_status java_failure(JNIEnv *env, ugly_ctx *ctx) ;

/* After a JNI call that can throw: a pending exception becomes the result. */
#define JCHECK(env, ctx) do { if ((*(env))->ExceptionCheck(env)) return java_failure((env), (ctx)) ; } while (0)

static int is(JNIEnv *env, jobject o, jclass c){
	return (o != NULL)&&(*env)->IsInstanceOf(env, o, c) ;
}


/* The foreign value behind o (a Foreign, or a Java interface wrapping one), as an owned handle; 0 if none. */
static ugly_status foreign_handle(JNIEnv *env, ugly_ctx *ctx, jobject o, ugly_handle *out){
	*out = UGLY_NO_HANDLE ;
	jobject f = o ;
	if (! is(env, o, J.Foreign)){
		f = (*env)->CallStaticObjectMethod(env, J.Bridge, J.b_foreignOf, o) ;
		JCHECK(env, ctx) ;
		if (f == NULL){
			return UGLY_OK ;
		}
	}
	return ugly_dup(ctx, (ugly_handle)(*env)->GetLongField(env, f, J.f_handle), out) ;
}


static ugly_status export_obj(JNIEnv *env, ugly_ctx *ctx, jobject o, ugly_handle *out){
	ugly_status st = foreign_handle(env, ctx, o, out) ;
	if ((st != UGLY_OK)||(*out != UGLY_NO_HANDLE)){
		return st ;     /* rule 5: a foreign value goes out as itself */
	}
	jlong id = (*env)->CallStaticLongMethod(env, J.Bridge, J.b_acquireId, o) ;
	JCHECK(env, ctx) ;
	jobject ref = (*env)->NewGlobalRef(env, o) ;
	return ugly_export(ctx, URT(), ref, (uintptr_t)id, out) ;
}


static ugly_status type_error(ugly_ctx *ctx, const char *to){
	char msg[96] ;
	snprintf(msg, sizeof(msg), "can't convert this Java value to %s", to) ;
	return fail(ctx, UGLY_ERR_TYPE, "ClassCastException", msg) ;
}


/* Bridge.kind's answers: what a Java value converts to. */
enum { K_NULL, K_BOOL, K_INTEGRAL, K_BIG, K_FLOATING, K_CHAR, K_STRING, K_BYTES, K_OBJECT } ;

static ugly_status to_val(JNIEnv *env, ugly_ctx *ctx, jobject o, ugly_kind want, ugly_val *out){
	*out = ugly_null() ;
	/* One call into Java, where the checks are cheap, rather than an IsInstanceOf per type. */
	jint k = K_NULL ;
	if (o != NULL){
		k = (*env)->CallStaticIntMethod(env, J.Bridge, J.b_kind, o) ;
		JCHECK(env, ctx) ;
	}
	int integral = (k == K_INTEGRAL), floating = (k == K_FLOATING), big = (k == K_BIG) ;
	int text = (k == K_STRING)||(k == K_CHAR) ;

	if (want == UGLY_ANY){
		if (o == NULL) return UGLY_OK ;
		else if (k == K_BOOL) want = UGLY_BOOL ;
		else if (integral||big) want = UGLY_INT ;
		else if (floating) want = UGLY_DOUBLE ;
		else if (text) want = UGLY_STRING ;
		else if (k == K_BYTES) want = UGLY_BYTES ;
		else want = UGLY_HANDLE ;
	}
	switch (want){
		case UGLY_NULL:
			return (o == NULL ? UGLY_OK : type_error(ctx, "null")) ;
		case UGLY_BOOL:
			if (k != K_BOOL) return type_error(ctx, "a boolean") ;
			*out = ugly_bool((*env)->CallBooleanMethod(env, o, J.bool_value)) ;
			JCHECK(env, ctx) ;
			return UGLY_OK ;
		case UGLY_INT:
			if (big){
				jint bits = (*env)->CallIntMethod(env, o, J.bi_bitLength) ;
				JCHECK(env, ctx) ;
				if (bits > 63){
					return fail(ctx, UGLY_ERR_TYPE, "ArithmeticException", "BigInteger doesn't fit in int64") ;
				}
			}
			else if (! integral){
				return type_error(ctx, "an integer") ;     /* no implicit narrowing from double */
			}
			*out = ugly_int((int64_t)(*env)->CallLongMethod(env, o, J.num_longValue)) ;
			JCHECK(env, ctx) ;
			return UGLY_OK ;
		case UGLY_DOUBLE:
			if (! (floating||integral||big)) return type_error(ctx, "a double") ;     /* integers widen */
			*out = ugly_double((double)(*env)->CallDoubleMethod(env, o, J.num_doubleValue)) ;
			JCHECK(env, ctx) ;
			return UGLY_OK ;
		case UGLY_STRING:
			if (k == K_CHAR){
				jchar ch = (*env)->CallCharMethod(env, o, J.char_value) ;
				JCHECK(env, ctx) ;
				jstring js = (*env)->NewString(env, &ch, 1) ;
				return string_val(env, ctx, js, out) ;
			}
			if (k != K_STRING) return type_error(ctx, "a string") ;
			return string_val(env, ctx, (jstring)o, out) ;
		case UGLY_BYTES: {
			if (k != K_BYTES) return type_error(ctx, "bytes") ;
			jsize n = (*env)->GetArrayLength(env, (jbyteArray)o) ;
			jbyte *b = (*env)->GetByteArrayElements(env, (jbyteArray)o, NULL) ;
			ugly_status st = ugly_val_set_bytes(ctx, out, (const uint8_t *)b, (size_t)n) ;
			(*env)->ReleaseByteArrayElements(env, (jbyteArray)o, b, JNI_ABORT) ;
			return st ;
		}
		case UGLY_HANDLE: {
			if (k != K_OBJECT){
				return type_error(ctx, "a handle") ;
			}
			ugly_handle h ;
			ugly_status st = export_obj(env, ctx, o, &h) ;
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
/* ugly_val -> Java                                                 */
/* ---------------------------------------------------------------- */

/* An argument (borrowed) as a local ref: ours resolve to the original (rule 5), foreign ones to a Foreign. */
static jobject to_java_raw(JNIEnv *env, ugly_ctx *ctx, ugly_val v, ugly_status *st) ;

static jobject to_java(JNIEnv *env, ugly_ctx *ctx, ugly_val v, ugly_status *st){
	jobject o = to_java_raw(env, ctx, v, st) ;
	if ((*st == UGLY_OK)&&(*env)->ExceptionCheck(env)){
		*st = java_failure(env, ctx) ;
		return NULL ;
	}
	return o ;
}


static jobject to_java_raw(JNIEnv *env, ugly_ctx *ctx, ugly_val v, ugly_status *st){
	*st = UGLY_OK ;
	switch (v.kind){
		case UGLY_NULL: return NULL ;
		case UGLY_BOOL: return (*env)->CallStaticObjectMethod(env, J.Boolean, J.bool_valueOf, (jboolean)v.as.b) ;
		case UGLY_INT: return (*env)->CallStaticObjectMethod(env, J.Long, J.long_valueOf, (jlong)v.as.i) ;
		case UGLY_DOUBLE: return (*env)->CallStaticObjectMethod(env, J.Double, J.double_valueOf, (jdouble)v.as.d) ;
		case UGLY_STRING: {
			jstring js = new_string(env, v.as.str.ptr, v.as.str.len) ;
			if (js == NULL){
				*st = fail(ctx, UGLY_ERR_TYPE, "TypeError", "string isn't valid UTF-8") ;
			}
			return js ;
		}
		case UGLY_BYTES: {
			jbyteArray b = (*env)->NewByteArray(env, (jsize)v.as.bytes.len) ;
			(*env)->SetByteArrayRegion(env, b, 0, (jsize)v.as.bytes.len, (const jbyte *)v.as.bytes.ptr) ;
			return b ;
		}
		case UGLY_HANDLE: {
			void *ref = NULL ;
			*st = ugly_resolve(ctx, URT(), v.as.h, &ref) ;
			if (*st != UGLY_OK){
				return NULL ;
			}
			if (ref != NULL){
				return (*env)->NewLocalRef(env, (jobject)ref) ;
			}
			ugly_handle h ;
			*st = ugly_dup(ctx, v.as.h, &h) ;
			if (*st != UGLY_OK){
				return NULL ;
			}
			return (*env)->CallStaticObjectMethod(env, J.Foreign, J.f_of, (jlong)h) ;     /* one per foreign value */
		}
		default:
			*st = UGLY_ERR_USAGE ;
			return NULL ;
	}
}


static jobjectArray to_java_array(JNIEnv *env, ugly_ctx *ctx, const ugly_val *args, size_t n, ugly_status *st){
	jobjectArray a = (*env)->NewObjectArray(env, (jsize)n, J.Object, NULL) ;     /* FindClass here walked the stack on every call */
	size_t i ;
	*st = UGLY_OK ;
	for (i = 0 ; i < n ; i++){
		jobject o = to_java(env, ctx, args[i], st) ;
		if (*st != UGLY_OK){
			return NULL ;
		}
		(*env)->SetObjectArrayElement(env, a, (jsize)i, o) ;
		(*env)->DeleteLocalRef(env, o) ;
	}
	return a ;
}


/* ---------------------------------------------------------------- */
/* Exceptions at the boundary                                       */
/* ---------------------------------------------------------------- */

/* The pending Java exception, escaping to the core (rule 7). Clears it. */
static ugly_status java_failure(JNIEnv *env, ugly_ctx *ctx){
	jthrowable e = (*env)->ExceptionOccurred(env) ;
	(*env)->ExceptionClear(env) ;
	jstring jmsg = (*env)->CallObjectMethod(env, e, J.t_getMessage) ;
	if (drop_exception(env)){
		jmsg = NULL ;
	}
	char *msg = c_string(env, jmsg) ;
	ugly_status st ;

	if (is(env, e, J.ForeignException)){
		jlong err = (*env)->CallLongMethod(env, e, J.fe_take) ;
		drop_exception(env) ;
		if (err != 0){
			free(msg) ;
			return ugly_restore_error(ctx, (ugly_error *)(intptr_t)err) ;     /* our own, unchanged */
		}
		/* new ForeignException(payload): rethrowing a foreign value, which is the payload itself. */
		jobject p = (*env)->CallObjectMethod(env, e, J.fe_payload) ;
		drop_exception(env) ;
		ugly_handle h ;
		if ((foreign_handle(env, ctx, p, &h) == UGLY_OK)&&(h != UGLY_NO_HANDLE)){
			free(msg) ;
			return ugly_raise_handle(ctx, URT(), "ForeignError", "rethrown foreign value", h) ;
		}
	}
	if (is(env, e, J.NotFound)){
		st = fail(ctx, UGLY_ERR_NOT_FOUND, "NoSuchFieldException", msg) ;
	}
	else if (is(env, e, J.NotCapable)){
		st = fail(ctx, UGLY_ERR_NOT_CAPABLE, "UnsupportedOperationException", msg) ;
	}
	else if (is(env, e, J.TypeMismatch)){
		st = fail(ctx, UGLY_ERR_TYPE, "ClassCastException", msg) ;
	}
	else {
		/* A Java exception: its simple class name, its message, and itself as the payload. */
		jobject cls = (*env)->CallObjectMethod(env, e, J.o_getClass) ;
		drop_exception(env) ;
		jstring jtype = (*env)->CallObjectMethod(env, cls, J.c_getSimpleName) ;
		if (drop_exception(env)){
			jtype = NULL ;
		}
		char *type = c_string(env, jtype) ;
		jlong id = (*env)->CallStaticLongMethod(env, J.Bridge, J.b_acquireId, e) ;
		drop_exception(env) ;
		st = ugly_raise_native(ctx, URT(), type, msg, (*env)->NewGlobalRef(env, e), (uintptr_t)id) ;
		free(type) ;
	}
	free(msg) ;
	return st ;
}


/*
 * Throws the ctx's current error into Java: the original exception if its
 * payload is a Java Throwable coming home (rule 7), otherwise a
 * ForeignException owning the error.
 */
static void throw_ugly(JNIEnv *env, ugly_ctx *ctx, ugly_status st){
	if (ugly_last_error(ctx) == NULL){
		fail(ctx, st, "Error", "operation failed") ;
	}
	ugly_error *err = ugly_take_error(ctx) ;
	if (err == NULL){
		jclass ise = (*env)->FindClass(env, "java/lang/IllegalStateException") ;
		(*env)->ThrowNew(env, ise, "ugly: operation failed") ;
		return ;
	}
	ugly_handle payload = UGLY_NO_HANDLE ;
	ugly_error_payload(ctx, err, &payload) ;
	void *ref = NULL ;
	if ((payload != UGLY_NO_HANDLE)&&(ugly_resolve(ctx, URT(), payload, &ref) == UGLY_OK)&&(ref != NULL)
		&&is(env, (jobject)ref, J.Throwable)){
		/* Take our local ref first: releasing the error may delete the global ref. */
		jthrowable original = (jthrowable)(*env)->NewLocalRef(env, (jobject)ref) ;
		ugly_release(ctx, payload) ;
		ugly_error_free(err) ;
		(*env)->Throw(env, original) ;
		return ;
	}
	ugly_status pst = UGLY_OK ;
	jobject p = (payload != UGLY_NO_HANDLE ? to_java(env, ctx, ugly_hval(payload), &pst) : NULL) ;
	if (pst != UGLY_OK){
		ugly_error *perr = ugly_take_error(ctx) ;     /* converting the payload failed: keep the original error */
		ugly_error_free(perr) ;
		p = NULL ;
	}
	ugly_release(ctx, payload) ;
	jstring type = new_string(env, ugly_error_type(err), strlen(ugly_error_type(err))) ;
	jstring message = new_string(env, ugly_error_message(err), strlen(ugly_error_message(err))) ;
	jstring language = new_string(env, ugly_error_language(err), strlen(ugly_error_language(err))) ;
	drop_exception(env) ;
	jobject e = (*env)->NewObject(env, J.ForeignException, J.fe_new, (jlong)(intptr_t)err, type, message, language, p) ;
	if (e == NULL){
		ugly_error_free(err) ;     /* NewObject threw (out of memory): that exception is pending */
		return ;
	}
	(*env)->Throw(env, (jthrowable)e) ;
}


/* ---------------------------------------------------------------- */
/* Natives behind ugly.Foreign                                      */
/* ---------------------------------------------------------------- */

/* This thread's ctx; a Java thread that has none gets one, deleted when it ends (ugly_ctx_thread). */
static ugly_ctx *native_ctx(JNIEnv *env){
	ugly_ctx *ctx = NULL ;
	if (ugly_ctx_thread(&ctx) != UGLY_OK){
		jclass ise = (*env)->FindClass(env, "java/lang/IllegalStateException") ;
		(*env)->ThrowNew(env, ise, "ugly: can't create a ctx on this thread") ;
	}
	return ctx ;
}


/* Java args -> owned ugly_vals; NULL (with a Java exception thrown) on failure. */
static ugly_val *native_args(JNIEnv *env, ugly_ctx *ctx, jobjectArray args, size_t *n){
	*n = (args != NULL ? (size_t)(*env)->GetArrayLength(env, args) : 0) ;
	ugly_val *a = calloc(*n + 1, sizeof(ugly_val)) ;
	size_t i ;
	for (i = 0 ; i < *n ; i++){
		jobject o = (*env)->GetObjectArrayElement(env, args, (jsize)i) ;
		ugly_status st = to_val(env, ctx, o, UGLY_ANY, &a[i]) ;
		(*env)->DeleteLocalRef(env, o) ;
		if (st != UGLY_OK){
			size_t j ;
			for (j = 0 ; j < i ; j++){
				ugly_val_release(ctx, &a[j]) ;
			}
			free(a) ;
			throw_ugly(env, ctx, st) ;
			return NULL ;
		}
	}
	return a ;
}


static void free_args(ugly_ctx *ctx, ugly_val *a, size_t n){
	size_t i ;
	for (i = 0 ; i < n ; i++){
		ugly_val_release(ctx, &a[i]) ;
	}
	free(a) ;
}


/* A core result as a Java object (and releases it), or throws. */
static jobject native_result(JNIEnv *env, ugly_ctx *ctx, ugly_status st, ugly_val *out){
	if (st != UGLY_OK){
		throw_ugly(env, ctx, st) ;
		return NULL ;
	}
	jobject o = to_java(env, ctx, *out, &st) ;
	ugly_val_release(ctx, out) ;
	if (st != UGLY_OK){
		throw_ugly(env, ctx, st) ;
		return NULL ;
	}
	return o ;
}


static jobject JNICALL n_call(JNIEnv *env, jclass cls, jlong h, jobjectArray args){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	size_t n ;
	ugly_val *a = (ctx != NULL ? native_args(env, ctx, args, &n) : NULL) ;
	if (a == NULL){
		return NULL ;
	}
	ugly_val out = ugly_null() ;
	ugly_status st = ugly_call(ctx, (ugly_handle)h, a, n, UGLY_ANY, &out) ;
	free_args(ctx, a, n) ;
	return native_result(env, ctx, st, &out) ;
}


static jobject JNICALL n_invoke(JNIEnv *env, jclass cls, jlong h, jstring method, jobjectArray args){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	size_t n ;
	ugly_val *a = (ctx != NULL ? native_args(env, ctx, args, &n) : NULL) ;
	if (a == NULL){
		return NULL ;
	}
	char *m = c_string(env, method) ;
	ugly_val out = ugly_null() ;
	ugly_status st = ugly_invoke(ctx, (ugly_handle)h, m, a, n, UGLY_ANY, &out) ;
	free(m) ;
	free_args(ctx, a, n) ;
	return native_result(env, ctx, st, &out) ;
}


/* Keyword arguments from Kw's arrays (Foreign.callKw, invokeKw): names malloc'd, values owned. */
static int native_kw(JNIEnv *env, ugly_ctx *ctx, jobjectArray names, jobjectArray values, ugly_opts *o, ugly_kwarg **kw){
	*o = (ugly_opts)UGLY_OPTS_INIT ;
	size_t n ;
	ugly_val *v = native_args(env, ctx, values, &n) ;
	if (v == NULL){
		*kw = NULL ;
		return -1 ;
	}
	*kw = calloc(n + 1, sizeof(ugly_kwarg)) ;
	size_t i ;
	for (i = 0 ; i < n ; i++){
		jstring s = (*env)->GetObjectArrayElement(env, names, (jsize)i) ;
		(*kw)[i].name = c_string(env, s) ;
		(*env)->DeleteLocalRef(env, s) ;
		(*kw)[i].value = v[i] ;
	}
	free(v) ;
	o->kwargs = *kw ;
	o->nkwargs = n ;
	return 0 ;
}


static void free_native_kw(ugly_ctx *ctx, ugly_opts *o, ugly_kwarg *kw){
	free((char *)o->hint) ;
	free((char *)o->context) ;
	size_t i ;
	for (i = 0 ; i < o->nkwargs ; i++){
		free((char *)kw[i].name) ;
		ugly_val_release(ctx, &kw[i].value) ;
	}
	free(kw) ;
}


/* The hint and context of Foreign.callOpts/invokeOpts, as malloc'd strings (NULL for none). */
static void native_opts(JNIEnv *env, jstring hint, jstring context, ugly_opts *o){
	o->hint = (hint != NULL ? c_string(env, hint) : NULL) ;
	o->context = (context != NULL ? c_string(env, context) : NULL) ;
}


static jobject JNICALL n_call_kw(JNIEnv *env, jclass cls, jlong h, jobjectArray args, jobjectArray names,
	jobjectArray values, jstring hint, jstring context){

	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	size_t n ;
	ugly_val *a = (ctx != NULL ? native_args(env, ctx, args, &n) : NULL) ;
	if (a == NULL){
		return NULL ;
	}
	ugly_opts o ;
	ugly_kwarg *kw ;
	if (native_kw(env, ctx, names, values, &o, &kw) != 0){
		free_args(ctx, a, n) ;
		return NULL ;
	}
	native_opts(env, hint, context, &o) ;
	ugly_val out = ugly_null() ;
	ugly_status st = ugly_call_opts(ctx, (ugly_handle)h, a, n, &o, UGLY_ANY, &out) ;
	free_args(ctx, a, n) ;
	free_native_kw(ctx, &o, kw) ;
	return native_result(env, ctx, st, &out) ;
}


static jobject JNICALL n_invoke_kw(JNIEnv *env, jclass cls, jlong h, jstring method, jobjectArray args,
	jobjectArray names, jobjectArray values, jstring hint, jstring context){

	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	size_t n ;
	ugly_val *a = (ctx != NULL ? native_args(env, ctx, args, &n) : NULL) ;
	if (a == NULL){
		return NULL ;
	}
	ugly_opts o ;
	ugly_kwarg *kw ;
	if (native_kw(env, ctx, names, values, &o, &kw) != 0){
		free_args(ctx, a, n) ;
		return NULL ;
	}
	native_opts(env, hint, context, &o) ;
	char *m = c_string(env, method) ;
	ugly_val out = ugly_null() ;
	ugly_status st = ugly_invoke_opts(ctx, (ugly_handle)h, m, a, n, &o, UGLY_ANY, &out) ;
	free(m) ;
	free_args(ctx, a, n) ;
	free_native_kw(ctx, &o, kw) ;
	return native_result(env, ctx, st, &out) ;
}


static jobject JNICALL n_get(JNIEnv *env, jclass cls, jlong h, jstring member){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	if (ctx == NULL){
		return NULL ;
	}
	char *m = c_string(env, member) ;
	ugly_val out = ugly_null() ;
	ugly_status st = ugly_get_member(ctx, (ugly_handle)h, m, UGLY_ANY, &out) ;
	free(m) ;
	return native_result(env, ctx, st, &out) ;
}


static void JNICALL n_set(JNIEnv *env, jclass cls, jlong h, jstring member, jobject value){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	if (ctx == NULL){
		return ;
	}
	ugly_val v ;
	ugly_status st = to_val(env, ctx, value, UGLY_ANY, &v) ;
	if (st == UGLY_OK){
		char *m = c_string(env, member) ;
		st = ugly_set_member(ctx, (ugly_handle)h, m, v) ;
		free(m) ;
		ugly_val_release(ctx, &v) ;
	}
	if (st != UGLY_OK){
		throw_ugly(env, ctx, st) ;
	}
}


static jobject JNICALL n_get_index(JNIEnv *env, jclass cls, jlong h, jint i){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	if (ctx == NULL){
		return NULL ;
	}
	ugly_val out = ugly_null() ;
	ugly_status st = ugly_get_index(ctx, (ugly_handle)h, (int64_t)i, UGLY_ANY, &out) ;
	return native_result(env, ctx, st, &out) ;
}


static void JNICALL n_set_index(JNIEnv *env, jclass cls, jlong h, jint i, jobject value){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	if (ctx == NULL){
		return ;
	}
	ugly_val v ;
	ugly_status st = to_val(env, ctx, value, UGLY_ANY, &v) ;
	if (st == UGLY_OK){
		st = ugly_set_index(ctx, (ugly_handle)h, (int64_t)i, v) ;
		ugly_val_release(ctx, &v) ;
	}
	if (st != UGLY_OK){
		throw_ugly(env, ctx, st) ;
	}
}


static jint JNICALL n_length(JNIEnv *env, jclass cls, jlong h){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	int64_t n = 0 ;
	ugly_status st = (ctx != NULL ? ugly_length(ctx, (ugly_handle)h, &n) : UGLY_ERR_USAGE) ;
	if ((ctx != NULL)&&(st != UGLY_OK)){
		throw_ugly(env, ctx, st) ;
	}
	return (jint)n ;
}


static jobjectArray JNICALL n_names(JNIEnv *env, jclass cls, jlong h){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	if (ctx == NULL){
		return NULL ;
	}
	ugly_handle names ;
	ugly_status st = ugly_member_names(ctx, (ugly_handle)h, &names) ;
	if (st != UGLY_OK){
		throw_ugly(env, ctx, st) ;
		return NULL ;
	}
	int64_t n = 0, i ;
	ugly_length(ctx, names, &n) ;
	jobjectArray a = (*env)->NewObjectArray(env, (jsize)n, J.String, NULL) ;
	for (i = 0 ; i < n ; i++){
		ugly_val s ;
		if (ugly_get_index(ctx, names, i, UGLY_STRING, &s) == UGLY_OK){
			jstring js = new_string(env, s.as.str.ptr, s.as.str.len) ;
			(*env)->SetObjectArrayElement(env, a, (jsize)i, js) ;
			(*env)->DeleteLocalRef(env, js) ;
			ugly_val_release(ctx, &s) ;
		}
	}
	ugly_release(ctx, names) ;
	return a ;
}


static jint JNICALL n_traits(JNIEnv *env, jclass cls, jlong h){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	uint32_t traits = 0 ;
	ugly_val tn ;
	if ((ctx != NULL)&&(ugly_describe(ctx, (ugly_handle)h, &traits, &tn) == UGLY_OK)){
		ugly_val_release(ctx, &tn) ;
	}
	return (jint)traits ;
}


static jboolean JNICALL n_same(JNIEnv *env, jclass cls, jlong a, jlong b){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	bool same = false ;
	if (ctx != NULL){
		ugly_same(ctx, (ugly_handle)a, (ugly_handle)b, &same) ;
	}
	return (jboolean)same ;
}


/* From a Cleaner thread: releases are thread-safe (deferred to the home runtime). */
static void JNICALL n_release(JNIEnv *env, jclass cls, jlong h){
	(void)env ; (void)cls ;
	ugly_release(ugly_current_ctx(), (ugly_handle)h) ;
}


static void JNICALL n_free_error(JNIEnv *env, jclass cls, jlong err){
	(void)env ; (void)cls ;
	ugly_error_free((ugly_error *)(intptr_t)err) ;
}


static ugly_rt *other(JNIEnv *env, ugly_ctx *ctx, jstring lang_s){
	char *lang = c_string(env, lang_s) ;
	ugly_rt *rt = NULL ;
	if (strcmp(lang, "java") == 0){
		jclass iae = (*env)->FindClass(env, "java/lang/IllegalArgumentException") ;
		(*env)->ThrowNew(env, iae, "ugly: use Java itself for Java") ;
	}
	else {
		ugly_status st = ugly_rt_get(ctx, lang, &rt) ;
		if (st != UGLY_OK){
			throw_ugly(env, ctx, st) ;
			rt = NULL ;
		}
	}
	free(lang) ;
	return rt ;
}


static jobject JNICALL n_module(JNIEnv *env, jclass cls, jstring lang, jstring name){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	ugly_rt *rt = (ctx != NULL ? other(env, ctx, lang) : NULL) ;
	if (rt == NULL){
		return NULL ;
	}
	char *n = c_string(env, name) ;
	ugly_handle h ;
	ugly_status st = ugly_rt_import(ctx, rt, n, &h) ;
	free(n) ;
	ugly_val v = ugly_hval(h) ;
	return native_result(env, ctx, st, &v) ;
}


static jobject JNICALL n_eval(JNIEnv *env, jclass cls, jstring lang, jstring code){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	ugly_rt *rt = (ctx != NULL ? other(env, ctx, lang) : NULL) ;
	if (rt == NULL){
		return NULL ;
	}
	char *c = c_string(env, code) ;
	ugly_val out = ugly_null() ;
	ugly_status st = ugly_rt_eval(ctx, rt, c, strlen(c), UGLY_ANY, &out) ;
	free(c) ;
	return native_result(env, ctx, st, &out) ;
}


static void JNICALL n_configure(JNIEnv *env, jclass cls, jstring lang, jstring key, jstring value){
	(void)cls ;
	ugly_ctx *ctx = native_ctx(env) ;
	if (ctx == NULL){
		return ;
	}
	char *l = c_string(env, lang), *k = c_string(env, key), *v = c_string(env, value) ;
	ugly_status st = ugly_configure(ctx, l, k, v) ;
	free(l) ;
	free(k) ;
	free(v) ;
	if (st != UGLY_OK){
		throw_ugly(env, ctx, st) ;
	}
}


static JNINativeMethod natives[] = {
	{ "call", "(J[Ljava/lang/Object;)Ljava/lang/Object;", (void *)n_call },
	{ "invoke", "(JLjava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;", (void *)n_invoke },
	{ "callOpts", "(J[Ljava/lang/Object;[Ljava/lang/String;[Ljava/lang/Object;Ljava/lang/String;Ljava/lang/String;)"
		"Ljava/lang/Object;", (void *)n_call_kw },
	{ "invokeOpts", "(JLjava/lang/String;[Ljava/lang/Object;[Ljava/lang/String;[Ljava/lang/Object;Ljava/lang/String;"
		"Ljava/lang/String;)Ljava/lang/Object;", (void *)n_invoke_kw },
	{ "get", "(JLjava/lang/String;)Ljava/lang/Object;", (void *)n_get },
	{ "set", "(JLjava/lang/String;Ljava/lang/Object;)V", (void *)n_set },
	{ "getIndex", "(JI)Ljava/lang/Object;", (void *)n_get_index },
	{ "setIndex", "(JILjava/lang/Object;)V", (void *)n_set_index },
	{ "length", "(J)I", (void *)n_length },
	{ "names", "(J)[Ljava/lang/String;", (void *)n_names },
	{ "traits", "(J)I", (void *)n_traits },
	{ "same", "(JJ)Z", (void *)n_same },
	{ "release", "(J)V", (void *)n_release },
	{ "freeError", "(J)V", (void *)n_free_error },
	{ "module", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/Object;", (void *)n_module },
	{ "eval", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/Object;", (void *)n_eval },
	{ "configure", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", (void *)n_configure },
} ;


/* ---------------------------------------------------------------- */
/* Operations                                                       */
/* ---------------------------------------------------------------- */

static JNIEnv *ENV(ugly_rt *rt){
	(void)rt ;
	return env_here() ;     /* this thread's, whichever thread it is */
}


/* Finishes a Bridge call that returned 'o' (or threw), converting with 'want'. */
static ugly_status out_val(JNIEnv *env, ugly_ctx *ctx, jobject o, ugly_kind want, ugly_val *out){
	if ((*env)->ExceptionCheck(env)){
		return java_failure(env, ctx) ;
	}
	return to_val(env, ctx, o, want, out) ;
}


#define FRAME_IN(env) (*(env))->PushLocalFrame((env), 64)
#define FRAME_OUT(env) frame_out(env)

/*
 * The JVM's own output (diagnostics, -Xcheck:jni) goes to stderr: stdout
 * belongs to the host. Under UGLY_JAVA_CHECK a JNI warning is fatal, once
 * the operation that caused it is over (so its Java stack gets printed).
 */
static volatile int jni_warned = 0 ;

#ifndef UGLY_BINDING
static jint JNICALL vm_print(FILE *f, const char *fmt, va_list ap){
	(void)f ;
	char buf[1024] ;
	int n = vsnprintf(buf, sizeof(buf), fmt, ap) ;
	if (strstr(buf, "WARNING in native method") != NULL){
		jni_warned = 1 ;
	}
	fputs(buf, stderr) ;
	return n ;
}
#endif


static jobject frame_out(JNIEnv *env){
	jobject r = (*env)->PopLocalFrame(env, NULL) ;
	if (jni_warned){
		fprintf(stderr, "ugly_java: JNI warning under UGLY_JAVA_CHECK, aborting\n") ;
		abort() ;
	}
	return r ;
}


static ugly_status jv_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out){
	JNIEnv *env = ENV(rt) ;
	FRAME_IN(env) ;
	jstring n = new_string(env, name, strlen(name)) ;
	jobject c = (*env)->CallStaticObjectMethod(env, J.Bridge, J.b_importClass, n) ;
	ugly_status st = out_val(env, ctx, c, UGLY_HANDLE, out) ;
	FRAME_OUT(env) ;
	return st ;
}


static ugly_status jv_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name){
	JNIEnv *env = ENV(rt) ;
	FRAME_IN(env) ;
	*traits = (uint32_t)(*env)->CallStaticIntMethod(env, J.Bridge, J.b_traits, (jobject)ref) ;
	if ((*env)->ExceptionCheck(env)){
		ugly_status st = java_failure(env, ctx) ;
		FRAME_OUT(env) ;
		return st ;
	}
	jobject name = (*env)->CallStaticObjectMethod(env, J.Bridge, J.b_typeName, (jobject)ref) ;
	ugly_status st = out_val(env, ctx, name, UGLY_STRING, type_name) ;
	FRAME_OUT(env) ;
	return st ;
}


static ugly_status jv_get_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want, ugly_val *out){
	JNIEnv *env = ENV(rt) ;
	FRAME_IN(env) ;
	jstring n = new_string(env, name, strlen(name)) ;
	jobject v = (*env)->CallStaticObjectMethod(env, J.Bridge, J.b_getMember, (jobject)ref, n) ;
	ugly_status st = out_val(env, ctx, v, want, out) ;
	FRAME_OUT(env) ;
	return st ;
}


static ugly_status jv_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	JNIEnv *env = ENV(rt) ;
	FRAME_IN(env) ;
	ugly_status st ;
	jobject jv = to_java(env, ctx, v, &st) ;
	if (st == UGLY_OK){
		jstring n = new_string(env, name, strlen(name)) ;
		(*env)->CallStaticVoidMethod(env, J.Bridge, J.b_setMember, (jobject)ref, n, jv) ;
		if ((*env)->ExceptionCheck(env)){
			st = java_failure(env, ctx) ;
		}
	}
	FRAME_OUT(env) ;
	return st ;
}


static ugly_status jv_delete_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name){
	JNIEnv *env = ENV(rt) ;
	FRAME_IN(env) ;
	ugly_status st = UGLY_OK ;
	jstring n = new_string(env, name, strlen(name)) ;
	(*env)->CallStaticVoidMethod(env, J.Bridge, J.b_deleteMember, (jobject)ref, n) ;
	if ((*env)->ExceptionCheck(env)){
		st = java_failure(env, ctx) ;
	}
	FRAME_OUT(env) ;
	return st ;
}


static ugly_status jv_member_names(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	JNIEnv *env = ENV(rt) ;
	FRAME_IN(env) ;
	jobjectArray a = (*env)->CallStaticObjectMethod(env, J.Bridge, J.b_memberNames, (jobject)ref) ;
	ugly_status st ;
	if ((*env)->ExceptionCheck(env)){
		st = java_failure(env, ctx) ;
	}
	else {
		jsize n = (*env)->GetArrayLength(env, a), i ;
		ugly_val *names = calloc((size_t)n + 1, sizeof(ugly_val)) ;
		jsize made = 0 ;
		st = UGLY_OK ;
		for (i = 0 ; (i < n)&&(st == UGLY_OK) ; i++){
			jobject s = (*env)->GetObjectArrayElement(env, a, i) ;
			st = string_val(env, ctx, (jstring)s, &names[i]) ;
			made = i + (st == UGLY_OK ? 1 : 0) ;
		}
		if (st == UGLY_OK){
			st = ugly_list_new(ctx, names, (size_t)n, out) ;
		}
		for (i = 0 ; i < made ; i++){
			ugly_val_release(ctx, &names[i]) ;
		}
		free(names) ;
	}
	FRAME_OUT(env) ;
	return st ;
}


static ugly_status jv_length(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out){
	JNIEnv *env = ENV(rt) ;
	jint n = (*env)->CallStaticIntMethod(env, J.Bridge, J.b_length, (jobject)ref) ;
	if ((*env)->ExceptionCheck(env)){
		return java_failure(env, ctx) ;
	}
	*out = (int64_t)n ;
	return UGLY_OK ;
}


static ugly_status jv_get_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want, ugly_val *out){
	JNIEnv *env = ENV(rt) ;
	if ((i < 0)||(i > 0x7FFFFFFF)){
		return fail(ctx, UGLY_ERR_NOT_FOUND, "IndexOutOfBoundsException", "index out of range") ;
	}
	FRAME_IN(env) ;
	jobject v = (*env)->CallStaticObjectMethod(env, J.Bridge, J.b_getIndex, (jobject)ref, (jint)i) ;
	ugly_status st = out_val(env, ctx, v, want, out) ;
	FRAME_OUT(env) ;
	return st ;
}


static ugly_status jv_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	JNIEnv *env = ENV(rt) ;
	if ((i < 0)||(i > 0x7FFFFFFF)){
		return fail(ctx, UGLY_ERR_NOT_FOUND, "IndexOutOfBoundsException", "index out of range") ;
	}
	FRAME_IN(env) ;
	ugly_status st ;
	jobject jv = to_java(env, ctx, v, &st) ;
	if (st == UGLY_OK){
		(*env)->CallStaticVoidMethod(env, J.Bridge, J.b_setIndex, (jobject)ref, (jint)i, jv) ;
		if ((*env)->ExceptionCheck(env)){
			st = java_failure(env, ctx) ;
		}
	}
	FRAME_OUT(env) ;
	return st ;
}


static ugly_status jv_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n){

	JNIEnv *env = ENV(rt) ;
	if (offset + remove > 0x7FFFFFFF){
		return fail(ctx, UGLY_ERR_NOT_FOUND, "IndexOutOfBoundsException", "index out of range") ;
	}
	FRAME_IN(env) ;
	ugly_status st = UGLY_OK ;
	jobjectArray a = to_java_array(env, ctx, insert, n, &st) ;
	if (st == UGLY_OK){
		(*env)->CallStaticVoidMethod(env, J.Bridge, J.b_splice, (jobject)ref, (jint)offset, (jint)remove, a) ;
		if ((*env)->ExceptionCheck(env)){
			st = java_failure(env, ctx) ;
		}
	}
	FRAME_OUT(env) ;
	return st ;
}


static ugly_status jv_call(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out){

	JNIEnv *env = ENV(rt) ;
	FRAME_IN(env) ;
	ugly_status st ;
	jobjectArray a = to_java_array(env, ctx, args, nargs, &st) ;
	if (st == UGLY_OK){
		jobject r = (*env)->CallStaticObjectMethod(env, J.Bridge, J.b_call, (jobject)ref, a) ;
		st = out_val(env, ctx, r, want, out) ;
	}
	FRAME_OUT(env) ;
	return st ;
}


static ugly_status jv_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out){

	JNIEnv *env = ENV(rt) ;
	FRAME_IN(env) ;
	ugly_status st ;
	jobjectArray a = to_java_array(env, ctx, args, nargs, &st) ;
	if (st == UGLY_OK){
		jstring m = new_string(env, method, strlen(method)) ;
		jobject r = (*env)->CallStaticObjectMethod(env, J.Bridge, J.b_invoke, (jobject)ref, m, a) ;
		st = out_val(env, ctx, r, want, out) ;
	}
	FRAME_OUT(env) ;
	return st ;
}


static ugly_status jv_new_instance(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_handle *out){

	JNIEnv *env = ENV(rt) ;
	FRAME_IN(env) ;
	ugly_status st ;
	jobjectArray a = to_java_array(env, ctx, args, nargs, &st) ;
	if (st == UGLY_OK){
		jobject r = (*env)->CallStaticObjectMethod(env, J.Bridge, J.b_newInstance, (jobject)ref, a) ;
		ugly_val v = ugly_null() ;
		st = out_val(env, ctx, r, UGLY_HANDLE, &v) ;
		if (st == UGLY_OK){
			*out = v.as.h ;
		}
	}
	FRAME_OUT(env) ;
	return st ;
}


/*
 * Call options (contract §11): Java takes a hint, the parameter types of the
 * overload to use; it has no keyword arguments and no call context.
 */
static ugly_status hint_opts(ugly_ctx *ctx, JNIEnv *env, const ugly_opts *o){
	if (o->nkwargs > 0){
		return fail(ctx, UGLY_ERR_NOT_SUPPORTED, "IllegalArgumentException", "Java has no keyword arguments") ;
	}
	if (o->context != NULL){
		return fail(ctx, UGLY_ERR_NOT_SUPPORTED, "IllegalArgumentException", "Java takes no call context") ;
	}
	jstring h = (o->hint != NULL ? new_string(env, o->hint, strlen(o->hint)) : NULL) ;
	(*env)->CallStaticVoidMethod(env, J.Bridge, J.b_hint, h) ;
	if (h != NULL){
		(*env)->DeleteLocalRef(env, h) ;
	}
	drop_exception(env) ;
	return UGLY_OK ;
}


static void clear_hint(JNIEnv *env){
	(*env)->CallStaticVoidMethod(env, J.Bridge, J.b_hint, NULL) ;
	drop_exception(env) ;
}


static ugly_status jv_call_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_kind want, ugly_val *out){

	JNIEnv *env = ENV(rt) ;
	ugly_status st = hint_opts(ctx, env, o) ;
	if (st == UGLY_OK){
		st = jv_call(ctx, rt, ref, args, nargs, want, out) ;
		clear_hint(env) ;
	}
	return st ;
}


static ugly_status jv_invoke_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, const ugly_opts *o, ugly_kind want, ugly_val *out){

	JNIEnv *env = ENV(rt) ;
	ugly_status st = hint_opts(ctx, env, o) ;
	if (st == UGLY_OK){
		st = jv_invoke(ctx, rt, ref, method, args, nargs, want, out) ;
		clear_hint(env) ;
	}
	return st ;
}


static ugly_status jv_new_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_handle *out){

	JNIEnv *env = ENV(rt) ;
	ugly_status st = hint_opts(ctx, env, o) ;
	if (st == UGLY_OK){
		st = jv_new_instance(ctx, rt, ref, args, nargs, out) ;
		clear_hint(env) ;
	}
	return st ;
}


static void jv_release(ugly_rt *rt, void *ref){
	(void)rt ;
	JNIEnv *env = env_here() ;
	(*env)->CallStaticVoidMethod(env, J.Bridge, J.b_releaseId, (jobject)ref) ;
	drop_exception(env) ;
	(*env)->DeleteGlobalRef(env, (jobject)ref) ;
}


/* ---------------------------------------------------------------- */
/* Lifecycle                                                        */
/* ---------------------------------------------------------------- */

static jclass global_class(JNIEnv *env, const char *name){
	jclass c = (*env)->FindClass(env, name) ;
	if (c == NULL){
		return NULL ;
	}
	jclass g = (*env)->NewGlobalRef(env, c) ;
	(*env)->DeleteLocalRef(env, c) ;
	return g ;
}


/* Looks up the classes and members we use; 0 on success (a Java exception may be pending otherwise). */
static int lookup(JNIEnv *env){
	if (! (J.Bridge = global_class(env, "ugly/Bridge"))) return -1 ;
	if (! (J.Foreign = global_class(env, "ugly/Foreign"))) return -1 ;
	if (! (J.ForeignException = global_class(env, "ugly/ForeignException"))) return -1 ;
	if (! (J.NotFound = global_class(env, "ugly/Bridge$NotFound"))) return -1 ;
	if (! (J.NotCapable = global_class(env, "ugly/Bridge$NotCapable"))) return -1 ;
	if (! (J.TypeMismatch = global_class(env, "ugly/Bridge$TypeMismatch"))) return -1 ;
	J.Throwable = global_class(env, "java/lang/Throwable") ;
	J.Class = global_class(env, "java/lang/Class") ;
	J.Boolean = global_class(env, "java/lang/Boolean") ;
	J.Long = global_class(env, "java/lang/Long") ;
	J.Integer = global_class(env, "java/lang/Integer") ;
	J.Short = global_class(env, "java/lang/Short") ;
	J.Byte = global_class(env, "java/lang/Byte") ;
	J.Double = global_class(env, "java/lang/Double") ;
	J.Float = global_class(env, "java/lang/Float") ;
	J.Character = global_class(env, "java/lang/Character") ;
	J.String = global_class(env, "java/lang/String") ;
	J.BigInteger = global_class(env, "java/math/BigInteger") ;
	J.Number = global_class(env, "java/lang/Number") ;
	J.ByteArray = global_class(env, "[B") ;
	J.Object = global_class(env, "java/lang/Object") ;

	const char *O = "Ljava/lang/Object;" ;
	char sig[256] ;
	J.b_importClass = (*env)->GetStaticMethodID(env, J.Bridge, "importClass", "(Ljava/lang/String;)Ljava/lang/Class;") ;
	snprintf(sig, sizeof(sig), "(%s)I", O) ;
	J.b_traits = (*env)->GetStaticMethodID(env, J.Bridge, "traits", sig) ;
	J.b_length = (*env)->GetStaticMethodID(env, J.Bridge, "length", sig) ;
	snprintf(sig, sizeof(sig), "(%s)Ljava/lang/String;", O) ;
	J.b_typeName = (*env)->GetStaticMethodID(env, J.Bridge, "typeName", sig) ;
	snprintf(sig, sizeof(sig), "(%sLjava/lang/String;)%s", O, O) ;
	J.b_getMember = (*env)->GetStaticMethodID(env, J.Bridge, "getMember", sig) ;
	snprintf(sig, sizeof(sig), "(%sLjava/lang/String;%s)V", O, O) ;
	J.b_setMember = (*env)->GetStaticMethodID(env, J.Bridge, "setMember", sig) ;
	snprintf(sig, sizeof(sig), "(%s)[Ljava/lang/String;", O) ;
	J.b_memberNames = (*env)->GetStaticMethodID(env, J.Bridge, "memberNames", sig) ;
	snprintf(sig, sizeof(sig), "(%sI)%s", O, O) ;
	J.b_getIndex = (*env)->GetStaticMethodID(env, J.Bridge, "getIndex", sig) ;
	snprintf(sig, sizeof(sig), "(%sI%s)V", O, O) ;
	J.b_setIndex = (*env)->GetStaticMethodID(env, J.Bridge, "setIndex", sig) ;
	snprintf(sig, sizeof(sig), "(%sLjava/lang/String;)V", O) ;
	J.b_deleteMember = (*env)->GetStaticMethodID(env, J.Bridge, "deleteMember", sig) ;
	snprintf(sig, sizeof(sig), "(%sII[%s)V", O, O) ;
	J.b_splice = (*env)->GetStaticMethodID(env, J.Bridge, "splice", sig) ;
	J.b_hint = (*env)->GetStaticMethodID(env, J.Bridge, "hint", "(Ljava/lang/String;)V") ;
	snprintf(sig, sizeof(sig), "(%s[%s)%s", O, O, O) ;
	J.b_call = (*env)->GetStaticMethodID(env, J.Bridge, "call", sig) ;
	J.b_newInstance = (*env)->GetStaticMethodID(env, J.Bridge, "newInstance", sig) ;
	snprintf(sig, sizeof(sig), "(%sLjava/lang/String;[%s)%s", O, O, O) ;
	J.b_invoke = (*env)->GetStaticMethodID(env, J.Bridge, "invoke", sig) ;
	snprintf(sig, sizeof(sig), "(%s)J", O) ;
	J.b_acquireId = (*env)->GetStaticMethodID(env, J.Bridge, "acquireId", sig) ;
	snprintf(sig, sizeof(sig), "(%s)V", O) ;
	J.b_releaseId = (*env)->GetStaticMethodID(env, J.Bridge, "releaseId", sig) ;
	snprintf(sig, sizeof(sig), "(%s)Lugly/Foreign;", O) ;
	J.b_foreignOf = (*env)->GetStaticMethodID(env, J.Bridge, "foreignOf", sig) ;
	snprintf(sig, sizeof(sig), "(%s)I", O) ;
	J.b_kind = (*env)->GetStaticMethodID(env, J.Bridge, "kind", sig) ;

	J.f_of = (*env)->GetStaticMethodID(env, J.Foreign, "of", "(J)Lugly/Foreign;") ;
	J.f_handle = (*env)->GetFieldID(env, J.Foreign, "handle", "J") ;
	J.fe_new = (*env)->GetMethodID(env, J.ForeignException, "<init>",
		"(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Object;)V") ;
	J.fe_take = (*env)->GetMethodID(env, J.ForeignException, "take", "()J") ;
	J.fe_payload = (*env)->GetMethodID(env, J.ForeignException, "payload", "()Ljava/lang/Object;") ;
	J.t_getMessage = (*env)->GetMethodID(env, J.Throwable, "getMessage", "()Ljava/lang/String;") ;
	J.o_getClass = (*env)->GetMethodID(env, J.Throwable, "getClass", "()Ljava/lang/Class;") ;
	J.c_getSimpleName = (*env)->GetMethodID(env, J.Class, "getSimpleName", "()Ljava/lang/String;") ;
	J.bool_valueOf = (*env)->GetStaticMethodID(env, J.Boolean, "valueOf", "(Z)Ljava/lang/Boolean;") ;
	J.bool_value = (*env)->GetMethodID(env, J.Boolean, "booleanValue", "()Z") ;
	J.long_valueOf = (*env)->GetStaticMethodID(env, J.Long, "valueOf", "(J)Ljava/lang/Long;") ;
	J.double_valueOf = (*env)->GetStaticMethodID(env, J.Double, "valueOf", "(D)Ljava/lang/Double;") ;
	J.num_longValue = (*env)->GetMethodID(env, J.Number, "longValue", "()J") ;
	J.num_doubleValue = (*env)->GetMethodID(env, J.Number, "doubleValue", "()D") ;
	J.char_value = (*env)->GetMethodID(env, J.Character, "charValue", "()C") ;
	J.bi_bitLength = (*env)->GetMethodID(env, J.BigInteger, "bitLength", "()I") ;
	if ((*env)->ExceptionCheck(env)){
		return -1 ;
	}
	return (*env)->RegisterNatives(env, J.Foreign, natives, sizeof(natives) / sizeof(natives[0])) ;
}


static ugly_status jv_init(ugly_ctx *ctx, const char *dir){
	(void)ctx ;
	JNIEnv *env = NULL ;
#ifdef UGLY_BINDING
	(void)dir ;
	env = env_here() ;     /* JNI_OnLoad set jvm: the host is java */
#else
	jsize n = 0 ;
	if ((JNI_GetCreatedJavaVMs(&jvm, 1, &n) != JNI_OK)||(n == 0)){
		/*
		 * We start the JVM. Class path: the helper jar next to this plugin,
		 * then ugly_configure's "classpath" values, UGLY_JAVA_CLASSPATH and
		 * CLASSPATH. Its "options" (-Xmx512m, -Dkey=value...) come after ours.
		 */
		const char *extra = getenv("UGLY_JAVA_CLASSPATH") ;
		const char *cp = getenv("CLASSPATH") ;
		const char *v ;
		size_t len = strlen(dir) + (extra ? strlen(extra) : 0) + (cp ? strlen(cp) : 0) + 64, i, nuser = 0 ;
		for (i = 0 ; (v = ugly_option("java", "classpath", i)) != NULL ; i++){
			len += strlen(v) + 1 ;
		}
		while (ugly_option("java", "options", nuser) != NULL){
			nuser++ ;
		}
		char *opt_cp = malloc(len) ;
		JavaVMOption *options = calloc(nuser + 6, sizeof(JavaVMOption)) ;
		if ((opt_cp == NULL)||(options == NULL)){
			free(opt_cp) ;
			free(options) ;
			return UGLY_ERR_NOMEM ;
		}
		size_t at = (size_t)snprintf(opt_cp, len, "-Djava.class.path=%s/ugly.jar", dir) ;
		for (i = 0 ; (v = ugly_option("java", "classpath", i)) != NULL ; i++){
			at += (size_t)snprintf(opt_cp + at, len - at, ":%s", v) ;
		}
		snprintf(opt_cp + at, len - at, "%s%s%s%s", extra ? ":" : "", extra ? extra : "", cp ? ":" : "", cp ? cp : "") ;
		int nopt = 0 ;
		options[nopt++].optionString = opt_cp ;
		options[nopt++].optionString = "-Xrs" ;     /* leave SIGINT, SIGTERM... to the host */
		options[nopt++].optionString = "-Dugly.embedded=true" ;     /* ugly.Foreign: natives are registered */
		options[nopt].optionString = "vfprintf" ;
		options[nopt++].extraInfo = (void *)vm_print ;
		const char *check = getenv("UGLY_JAVA_CHECK") ;     /* set and not empty */
		if ((check != NULL)&&(*check != '\0')){
			options[nopt++].optionString = "-Xcheck:jni" ;
		}
		for (i = 0 ; i < nuser ; i++){
			options[nopt++].optionString = (char *)ugly_option("java", "options", i) ;
		}
		JavaVMInitArgs args ;
		args.version = JNI_VERSION_10 ;
		args.nOptions = nopt ;
		args.options = options ;
		args.ignoreUnrecognized = JNI_FALSE ;
		jint rc = JNI_CreateJavaVM(&jvm, (void **)&env, &args) ;
		free(opt_cp) ;
		free(options) ;
		if (rc != JNI_OK){
			return UGLY_ERR_INTERNAL ;
		}
	}
	else {
		env = env_here() ;     /* the host is Java */
	}
#endif
	if (lookup(env) != 0){
		if ((*env)->ExceptionCheck(env)){
			(*env)->ExceptionDescribe(env) ;
			(*env)->ExceptionClear(env) ;
		}
		return UGLY_ERR_INTERNAL ;
	}
	return UGLY_OK ;
}


static ugly_status jv_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	(void)ctx ;
	jrt *r = calloc(1, sizeof(jrt)) ;
	if (r == NULL){
		return UGLY_ERR_NOMEM ;
	}
	r->urt = rt ;
	env_here() ;     /* attaches this thread if it isn't yet */
	the_rt = r ;
	*native = r ;
	return UGLY_OK ;
}


/* The host's JVM, as seen from the attaching thread ('existing' is its JNIEnv). */
static ugly_status jv_rt_attach(ugly_ctx *ctx, ugly_rt *rt, void *existing, void **native){
	if (existing == NULL){
		return UGLY_ERR_USAGE ;
	}
	return jv_rt_new(ctx, rt, native) ;
}


static ugly_status jv_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	(void)ctx ; (void)rt ;
	if (the_rt == native){
		the_rt = NULL ;
	}
	free(native) ;
	return UGLY_OK ;
}


static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = UGLY_CAP_ATTACH | UGLY_CAP_ANY_THREAD,     /* the JVM is multi-threaded; JNIEnv per thread */
	.name = "java",
	.init = jv_init,
	.rt_new = jv_rt_new,
	.options = (const char *const []){ "classpath", "options", NULL },
	.rt_attach = jv_rt_attach,
	.rt_delete = jv_rt_delete,
	.import = jv_import,
	.describe = jv_describe,
	.get_member = jv_get_member,
	.set_member = jv_set_member,
	.member_names = jv_member_names,
	.length = jv_length,
	.get_index = jv_get_index,
	.set_index = jv_set_index,
	.delete_member = jv_delete_member,
	.splice = jv_splice,
	.call_opts = jv_call_opts,
	.invoke_opts = jv_invoke_opts,
	.new_opts = jv_new_opts,
	.call = jv_call,
	.invoke = jv_invoke,
	.new_instance = jv_new_instance,
	.release = jv_release,
} ;


const ugly_plugin *ugly_plugin_java(void){
	return &plugin ;
}


#ifdef UGLY_BINDING
/* ---------------------------------------------------------------- */
/* The binding: ugly.Ugly from a plain java program                 */
/* ---------------------------------------------------------------- */

/*
 * Built from this same file as libugly_jni.so, without libjvm: the JVM is
 * the host's. ugly.Foreign's static initializer loads it, and JNI_OnLoad
 * registers this plugin as "java" and attaches to the JVM on the loading
 * thread, which then owns the ctx and the runtime.
 *
 * Teardown is at JVMTI's VMDeath. The java launcher's DestroyJavaVM runs on
 * the thread that ran main, and so does System.exit called from it, so the
 * ctx is still there and JNI still works; shutdown hooks would run on new
 * threads, which can't touch it. If the JVM ends from another thread, UGLY
 * is left as it is, like any process exit.
 */
#include <jvmti.h>

static ugly_ctx *b_ctx ;
static ugly_lang *b_lang ;
static ugly_rt *b_rt ;
static int b_owns_ctx ;


static void JNICALL b_vm_death(jvmtiEnv *jvmti, JNIEnv *env){
	(void)jvmti ; (void)env ;     /* operations get this thread's JNIEnv themselves (env_here) */
	if ((b_rt == NULL)||(ugly_current_ctx() != b_ctx)){
		return ;
	}
	ugly_rt_delete(b_ctx, b_rt) ;
	ugly_lang_unload(b_ctx, b_lang) ;
	if (b_owns_ctx){
		ugly_ctx_delete(b_ctx) ;     /* also deletes the runtimes Ugly.module created */
	}
	b_rt = NULL ;
}


static jint attach_failed(const char *why){
	fprintf(stderr, "ugly: can't attach to this JVM: %s\n", why) ;
	return JNI_ERR ;     /* System.loadLibrary throws UnsatisfiedLinkError */
}


JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved){
	(void)reserved ;
	if (b_rt != NULL){
		return JNI_VERSION_10 ;
	}
	jvm = vm ;
	JNIEnv *env = env_here() ;
	b_ctx = ugly_current_ctx() ;
	if (b_ctx == NULL){
		if (ugly_ctx_new(&b_ctx) != UGLY_OK){
			return attach_failed("can't create a context") ;
		}
		b_owns_ctx = 1 ;
	}
	ugly_status st = ugly_lang_register(b_ctx, &plugin, &b_lang) ;
	if (st == UGLY_OK){
		st = ugly_rt_attach(b_ctx, b_lang, env, &b_rt) ;
	}
	if (st != UGLY_OK){
		const ugly_error *e = ugly_last_error(b_ctx) ;
		return attach_failed(e ? ugly_error_message(e) : "unknown error") ;
	}
	jvmtiEnv *jvmti = NULL ;
	jvmtiEventCallbacks cb ;
	memset(&cb, 0, sizeof(cb)) ;
	cb.VMDeath = b_vm_death ;
	if (((*vm)->GetEnv(vm, (void **)&jvmti, JVMTI_VERSION_1_2) != JNI_OK)
		||((*jvmti)->SetEventCallbacks(jvmti, &cb, sizeof(cb)) != JVMTI_ERROR_NONE)
		||((*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH, NULL) != JVMTI_ERROR_NONE)){
		fprintf(stderr, "ugly: no JVMTI, so no teardown at exit\n") ;
	}
	return JNI_VERSION_10 ;
}
#endif
