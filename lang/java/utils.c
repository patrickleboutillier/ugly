#include <stdlib.h>
#include <string.h>
#include "ugly_java_priv.h"


JNIEnv *get_env(ugly_context *ctx, ugly_java_runtime *jrt){
    JNIEnv *env ;

	jint rc = (*(jrt->jvm))->AttachCurrentThread(jrt->jvm, ((void **)&env), NULL) ;
	if (rc < 0){
		ugly_context_set_error(ctx, UGLY_RUNTIME_ERROR, "Can't attach current thread to Java VM") ;
		return NULL ;
	}

    return env ;
}


/* Use this to check for exceptions without recuperating anything from the exception. */
static char check_exception(JNIEnv *env){
	if ((*(env))->ExceptionCheck(env)){
		if (ugly_debug_on(1)){
			(*env)->ExceptionDescribe(env) ;
		}
		(*(env))->ExceptionClear(env) ;
		return 1 ;
	}
	return 0 ;
}


/* If an exception if pending, extract the class name and message and setup
 * the context error.
 */
/*
char process_exception(mlp_context *ctx, mlp_java_runtime *jrt, JNIEnv *env, mlp_error err, const char *fmt, ...){
	if ((*env)->ExceptionCheck(env)){
		jthrowable e = (*env)->ExceptionOccurred(env) ;
		if (mlp_debug_on(1)){
			(*env)->ExceptionDescribe(env) ;
		}
		(*env)->ExceptionClear(env) ;

		va_list va ;
		va_start(va, fmt) ;
		char imsg[MLP_MAX_ERROR_MSG_LENGTH] ;
		vsnprintf(imsg, MLP_MAX_ERROR_MSG_LENGTH - 1, fmt, va) ;
		va_end(va) ;

		jclass eclass = (*env)->GetObjectClass(env, e) ;
		jstring cname = (*env)->CallObjectMethod(env, eclass, jrt->class_getName) ;
		if ((*(env))->ExceptionCheck(env)){
			mlp_context_set_error(ctx, err, "%s:\n  (Can't call getName() method of jclass object)", imsg) ;
			(*env)->DeleteLocalRef(env, eclass) ;
			(*env)->DeleteLocalRef(env, e) ;
			return 0 ;
		}

		jstring msg = (*env)->CallObjectMethod(env, e, jrt->throwable_getMessage) ;
		if ((*(env))->ExceptionCheck(env)){
			mlp_context_set_error(ctx, err, "%s:\n  (Can't call getMessage() method on jthrowable object)", imsg) ;
			(*env)->DeleteLocalRef(env, eclass) ;
			(*env)->DeleteLocalRef(env, e) ;
			return 0 ;
		}

		const char *cnamestr = (const char *)((*env)->GetStringUTFChars(env, cname, NULL)) ;
		const char *msgstr = NULL ;
		if (msg != NULL){
			msgstr = (const char *)((*env)->GetStringUTFChars(env, msg, NULL)) ;
		}
		mlp_context_set_error(ctx, err, "%s:\n  Exception (%s): %s", imsg, cnamestr, msgstr) ;
		if (msg != NULL){
			(*env)->ReleaseStringUTFChars(env, msg, msgstr) ;
		}
		(*env)->ReleaseStringUTFChars(env, cname, cnamestr) ;
		(*env)->DeleteLocalRef(env, cname) ;
		(*env)->DeleteLocalRef(env, msg) ;
		(*env)->DeleteLocalRef(env, e) ;
		return 0 ;
	}

	return 1 ;
}
*/


static jclass find_class(ugly_context *ctx, JNIEnv *env, const char *class){
	char *mclass = strdup(class) ;
	int i = 0, l = strlen(mclass) ;
	for (; i < l ; i++){
		if (mclass[i] == '.'){
			mclass[i] = '/' ;
		}
	}
	jclass cl = (*env)->FindClass(env, mclass) ;
	free(mclass) ;
	if (check_exception(env)){
		ugly_context_set_error(ctx, UGLY_RUNTIME_ERROR, "Can't find Java class '%s'", class) ;
		return NULL ;
	}

	return cl ;
}


/* Used to load classes ugly works with */
jclass load_class(ugly_context *ctx, JNIEnv *env, const char *class){
	
	jclass cl = find_class(ctx, env, class) ;
	RETURN_NULL_IF_NULL(cl) ;

	jclass c = (*(env))->NewGlobalRef(env, cl) ;

	return c ;
}


jmethodID get_method(ugly_context *ctx, JNIEnv *env, const char *cname, jclass class, const char *name, const char *proto){
    jmethodID m = (*(env))->GetMethodID(env, class, name, proto) ;
    if (check_exception(env)){
        ugly_context_set_error(ctx, UGLY_RUNTIME_ERROR,
            "Can't get method '%s%s' for Java class %s", name, proto, cname) ;
        return NULL ;
    }
	return m ;
}


jmethodID get_static_method(ugly_context *ctx, JNIEnv *env, const char *cname, jclass class, const char *name, const char *proto){
    jmethodID m = (*(env))->GetStaticMethodID(env, class, name, proto) ;
    if (check_exception(env)){
        ugly_context_set_error(ctx, UGLY_RUNTIME_ERROR,
            "Can't get method '%s%s' for Java class %s", name, proto, cname) ;
        return NULL ;
    }
	return m ;
}


ugly_value *java_call_helper_function(ugly_context *ctx, ugly_runtime *rt, const char *func,
	int nb_args, ugly_value **args){

	ugly_java_runtime *_rt = (ugly_java_runtime *)rt->handle ;
	JNIEnv *env = get_env(ctx, _rt) ;
	RETURN_NULL_IF_NULL(env) ;

	jobject jctxptr = (*(env))->NewObject(env, _rt->pointer, _rt->pointer_new, ctx) ;
    check_exception(env) ;
	jobject jctx = (*(env))->CallStaticObjectMethod(env, _rt->context, _rt->context_wrap, jctxptr) ;
    check_exception(env) ;

	jobject jrtptr = (*(env))->NewObject(env, _rt->pointer, _rt->pointer_new, rt) ;
    check_exception(env) ;
	jobject jrt = (*(env))->CallStaticObjectMethod(env, _rt->runtime, _rt->runtime_wrap, jrtptr) ;
    check_exception(env) ;

	jobjectArray jargs = (*(env))->NewObjectArray(env, nb_args, _rt->value, NULL) ;
	ugly_debug(3, "allo") ;
    check_exception(env) ;
    int i = 0 ;
    for (i = 0; i < nb_args ; i++){
		jobject jvalptr = (*(env))->NewObject(env, _rt->pointer, _rt->pointer_new, args[i]) ;
	    check_exception(env) ;
		jobject arg = (*(env))->CallStaticObjectMethod(env, _rt->value, _rt->value_wrap, jvalptr) ;
	    check_exception(env) ;
		(*(env))->SetObjectArrayElement(env, jargs, i, arg) ;
	    check_exception(env) ;
	}
	
	jstring jfunc = (*(env))->NewStringUTF(env, func) ;
	jobject jretval = (*(env))->CallStaticObjectMethod(env, _rt->helper, _rt->helper_dispatch,
		jctx, jrt, jfunc, jargs) ;
    check_exception(env) ;
	jobject jretvalptr = (*(env))->CallObjectMethod(env, jretval, _rt->value_getptr) ;
    check_exception(env) ;

	jlong val = (*(env))->CallStaticLongMethod(env, _rt->pointer, _rt->pointer_nativevalue, jretvalptr) ;
    check_exception(env) ;
	ugly_debug(3, "allo2 %p", val) ;
	ugly_debug(3, "%s", ugly_value_to_string((ugly_value *)val)) ;

    return (ugly_value *)val ;
}


ugly_value *java_call_helper_function_v(ugly_context *ctx, ugly_runtime *rt, const char *func, 
	int nb_args, ...){

    va_list ap ;
    va_start(ap, nb_args) ;

	ugly_value **args = UGLY_MALLOC_N(ugly_value **, nb_args) ;

	int i = 0 ;
	for (i = 0; i < nb_args ; i++){
		ugly_value *arg = va_arg(ap, ugly_value *) ;
		args[i] = arg ;
	}

	ugly_value *ret = java_call_helper_function(ctx, rt, func, nb_args, args) ;
	free(args) ;

	return ret ;
}

