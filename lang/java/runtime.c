#include <stdlib.h>
#include <string.h>
#include "ugly_java_priv.h"


static ugly_java_runtime *init_jrt(ugly_context *ctx, JavaVM *jvm, JNIEnv *env){
	jclass class = load_class(ctx, env, "java/lang/Class") ;
	RETURN_NULL_IF_NULL(class) ;
	jmethodID class_getName = get_method(ctx, env, "java/lang/Class", class,
		"getName", "()Ljava/lang/String;") ;
	RETURN_NULL_IF_NULL(class_getName) ;

	jclass throwable = load_class(ctx, env, "java/lang/Throwable") ;
	RETURN_NULL_IF_NULL(throwable) ;
	jmethodID throwable_getMessage = get_method(ctx, env, "java/lang/Throwable", throwable,
		"getMessage", "()Ljava/lang/String;") ;
	RETURN_NULL_IF_NULL(throwable_getMessage) ;

	jclass pointer = load_class(ctx, env, "com/sun/jna/Pointer") ;
	RETURN_NULL_IF_NULL(pointer) ;
	jmethodID pointer_new = get_method(ctx, env, "com/sun/jna/Pointer", pointer,
		"<init>", "(J)V") ;
	RETURN_NULL_IF_NULL(pointer_new) ;
	jmethodID pointer_nativevalue = get_static_method(ctx, env, "com/sun/jna/Pointer", pointer,
		"nativeValue", "(Lcom/sun/jna/Pointer;)J") ;
	RETURN_NULL_IF_NULL(pointer_new) ;

	jclass context = load_class(ctx, env, "ugly/UglyContext") ;
	RETURN_NULL_IF_NULL(context) ;
	jmethodID context_wrap = get_static_method(ctx, env, "ugly/UglyContext", context,
		"wrap", "(Lcom/sun/jna/Pointer;)Lugly/UglyContext;") ;
	RETURN_NULL_IF_NULL(context_wrap) ;

	jclass runtime = load_class(ctx, env, "ugly/UglyRuntime") ;
	RETURN_NULL_IF_NULL(runtime) ;
	jmethodID runtime_wrap = get_static_method(ctx, env, "ugly/UglyRuntime", runtime,
		"wrap", "(Lcom/sun/jna/Pointer;)Lugly/UglyRuntime;") ;
	RETURN_NULL_IF_NULL(runtime_wrap) ;

	jclass value = load_class(ctx, env, "ugly/UglyValue") ;
	RETURN_NULL_IF_NULL(value) ;
	jmethodID value_wrap = get_static_method(ctx, env, "ugly/UglyValue", value,
		"wrap", "(Lcom/sun/jna/Pointer;)Lugly/UglyValue;") ;
	RETURN_NULL_IF_NULL(value_wrap) ;
	jmethodID value_getptr = get_method(ctx, env, "ugly/UglyValue", value,
		"getPtr", "()Lcom/sun/jna/Pointer;") ;
	RETURN_NULL_IF_NULL(value_wrap) ;

	jclass helper = load_class(ctx, env, "ugly/helper") ;
	RETURN_NULL_IF_NULL(helper) ;
	jmethodID helper_dispatch = get_static_method(ctx, env, "ugly/helper", helper,
		"dispatch", "(Lugly/UglyContext;Lugly/UglyRuntime;Ljava/lang/String;[Lugly/UglyValue;)Lugly/UglyValue;") ;
	RETURN_NULL_IF_NULL(helper_dispatch) ;


	ugly_java_runtime *jrt = UGLY_CALLOC(ugly_java_runtime) ;
	jrt->jvm = jvm ;
	jrt->class = class ;
	jrt->class_getName = class_getName ;
	jrt->throwable = throwable ;
	jrt->throwable_getMessage = throwable_getMessage ;
	jrt->pointer = pointer ;
	jrt->pointer_new = pointer_new ;
	jrt->pointer_nativevalue = pointer_nativevalue ;
	jrt->context = context ;
	jrt->context_wrap = context_wrap ;
	jrt->runtime = runtime ;
	jrt->runtime_wrap = runtime_wrap ; 
	jrt->value = value ;
	jrt->value_wrap = value_wrap ;
	jrt->value_getptr = value_getptr ;
	jrt->helper = helper ;
	jrt->helper_dispatch = helper_dispatch ;

	return jrt ;
}


void *_ugly_java_runtime_new(ugly_context *ctx, ugly_language *lang){
	JavaVMInitArgs vm_args ;
	vm_args.version = JNI_VERSION_1_2 ;
	vm_args.options = (JavaVMOption *)malloc(1 * sizeof(JavaVMOption)) ;
	vm_args.nOptions = 0 ;
	vm_args.ignoreUnrecognized = JNI_FALSE ;

	const char *classpath = getenv("CLASSPATH") ;
	const char *dir = ugly_language_get_dir(lang) ;
	if (classpath == NULL){
		classpath = "" ;
	}
	char *cp = (char *)malloc((strlen(dir) + strlen(classpath) + 64) * sizeof(char)) ;
	sprintf(cp, "-Djava.class.path=%s:%s", dir, classpath) ;
	vm_args.options[vm_args.nOptions++].optionString = cp ;

	JavaVM *jvm = NULL ;
	JNIEnv *env = NULL ;
	jint res = JNI_CreateJavaVM(&jvm, (void **)&(env), &vm_args) ;
	if (res < 0) {
		ugly_context_set_error(ctx, UGLY_RUNTIME_ERROR, "Can't create Java VM") ;
		return NULL ;
	}

	ugly_java_runtime *jrt = init_jrt(ctx, jvm, env) ;
	RETURN_NULL_IF_NULL(jrt) ;

	return (void *)jrt ;
}


void _ugly_java_runtime_load_library(ugly_context *ctx, ugly_runtime *rt, const char *lib){
   	ugly_context_set_error(ctx, UGLY_NOT_SUPPORTED, "Java loads class/jar libraries automatically as they are required.") ;
}


ugly_value *_ugly_java_runtime_call_function(ugly_context *ctx, ugly_runtime *rt, ugly_type rtype, 
	const char *func, ugly_value **args, int nb_args, const char *hint){
	ugly_debug(3, "Calling java function ugly.helper.call_function") ;

	int extra = 2 ;
	ugly_value **new_args = UGLY_MALLOC_N(ugly_value *, nb_args + extra) ;
	new_args[0] = ugly_value_new_long(rtype) ;
	new_args[1] = ugly_value_new_string(func) ;
	int i = 0 ;
	for (i = 0 ; i < nb_args ; i++){
		new_args[extra + i] = args[i] ;
	}

	ugly_value *ret = java_call_helper_function(ctx, rt, "call_function",
	    nb_args + extra, new_args) ;

	for (i = 0 ; i < extra ; i++){
		ugly_value_delete(new_args[i]) ;
	}
	free(new_args) ;

	ugly_debug(3, "Called java function ugly.helper.call_function") ;
	ugly_debug(3, "%s", ugly_value_to_string(ret)) ;

	return ret ;
}
