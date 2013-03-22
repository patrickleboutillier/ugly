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
	jmethodID new_pointer ;

	jclass context ;
	jmethodID context_wrap ;

	jclass runtime ;
	jmethodID runtime_wrap ;

	jclass value ;
	jmethodID value_wrap ;

	jclass helper ;
	jmethodID helper_dispatch ;
} ;
typedef struct _ugly_java_runtime ugly_java_runtime ;


JNIEnv *get_env(ugly_context *ctx, ugly_java_runtime *jrt) ;

jclass load_class(ugly_context *ctx, JNIEnv *env, const char *class) ;
jmethodID get_method(ugly_context *ctx, JNIEnv *env, const char *cname, jclass class, const char *name, const char *proto) ;
jmethodID get_static_method(ugly_context *ctx, JNIEnv *env, const char *cname, jclass class, const char *name, const char *proto) ;
jstring jstring_new(ugly_context *ctx, JNIEnv *env, const char *s) ;
void jstring_delete(JNIEnv *env, jstring s) ; 

jobject java_MLPValue_new(ugly_context *ctx, ugly_runtime *rt, JNIEnv *env, ugly_value *v) ;
jobjectArray java_MLPValue_array_new(ugly_context *ctx, ugly_runtime *rt, JNIEnv *env, ugly_value **args, int nb_args) ;
void java_MLPValue_array_delete(JNIEnv *env, jobjectArray vargs, int nb_args) ;
ugly_value *ugly_value_new(ugly_context *ctx, ugly_runtime *rt, JNIEnv *env, jobject v) ;
jobject java_MLPJavaObject_new(ugly_context *ctx, ugly_runtime *rt, JNIEnv *env, ugly_object *obj) ;


#endif
