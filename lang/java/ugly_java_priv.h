#ifndef _UGLY_JAVA_PRIV_H_
#define _UGLY_JAVA_PRIV_H_

#include "../../lib/c/ugly_priv.h"

#include "jni.h"


struct _ugly_java_runtime {
	JavaVM *jvm ;

    jclass class ;
    jmethodID class_getName ;

    jclass throwable ;
    jmethodID throwable_getMessage ;

	jclass pointer ;
	jmethodID pointer_new ;
	jmethodID pointer_nativevalue ;

	jclass context ;
	jmethodID context_wrap ;

	jclass runtime ;
	jmethodID runtime_wrap ;

	jclass value ;
	jmethodID value_wrap ;
	jmethodID value_getptr ;

	jclass helper ;
	jmethodID helper_dispatch ;
} ;
typedef struct _ugly_java_runtime ugly_java_runtime ;


JNIEnv *get_env(ugly_context *ctx, ugly_java_runtime *jrt) ;

jclass load_class(ugly_context *ctx, JNIEnv *env, const char *class) ;
jmethodID get_method(ugly_context *ctx, JNIEnv *env, const char *cname, jclass class, const char *name, const char *proto) ;
jmethodID get_static_method(ugly_context *ctx, JNIEnv *env, const char *cname, jclass class, const char *name, const char *proto) ;

ugly_value *java_call_helper_function(ugly_context *ctx, ugly_runtime *rt, const char *func,
	int nb_args, ugly_value **args) ;
ugly_value *java_call_helper_function_v(ugly_context *ctx, ugly_runtime *rt, const char *func, 
	int nb_args, ...) ;


#endif
