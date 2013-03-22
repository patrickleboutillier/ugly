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
		"<init>", "(J)Lcom/sun/jna/Pointer;") ;
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

	jclass helper = load_class(ctx, env, "ugly/helper") ;
	RETURN_NULL_IF_NULL(value) ;
	jmethodID helper_dispatch = get_static_method(ctx, env, "ugly/helper", value,
		"dispatch", "(Lugly/UglyContext;Lugly/UglyRuntime;[Lugly/UglyValue;)Lugly/UglyValue;") ;
	RETURN_NULL_IF_NULL(value_wrap) ;


	ugly_java_runtime *jrt = UGLY_CALLOC(ugly_java_runtime) ;
	jrt->jvm = jvm ;
	jrt->class = class ;
	jrt->class_getName = class_getName ;
	jrt->throwable = throwable ;
	jrt->throwable_getMessage = throwable_getMessage ;
	jrt->context = context ;
	jrt->context_wrap = context_wrap ;
	jrt->runtime = runtime ;
	jrt->runtime_wrap = runtime_wrap ;
	jrt->value = value ;
	jrt->value_wrap = value_wrap ;
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
	sprintf(cp, "-Djava.class.path=%s:%s/helper.jar", dir, classpath) ;
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
}


ugly_value *_ugly_java_runtime_call_function(ugly_context *ctx, ugly_runtime *rt, ugly_type rtype, 
	const char *func, ugly_value **args, int nb_args, const char *hint){
}


/*
void *_mlp_java_runtime_new(mlp_context *ctx, mlp_language *lang){
}


void _mlp_java_runtime_load_library(mlp_context *ctx, mlp_runtime *rt, const char *lib){
   	mlp_context_set_error(ctx, MLP_NOT_SUPPORTED, "Java loads class/jar libraries automatically as they are required.") ;
}


void _mlp_java_runtime_delete(mlp_context *ctx, mlp_runtime *rt){
	mlp_java_runtime *jrt = (mlp_java_runtime *)rt->handle ;
	JavaVM *jvm = jrt->jvm ;
	mlp_debug(2, "Destroying JavaVM...") ;
	jint rc = (*jvm)->DestroyJavaVM(jvm) ;
	mlp_debug(3, "JavaVM destroyed") ;
	if (rc < 0){
    	mlp_context_set_error(ctx, MLP_RUNTIME_ERROR, "Error destroying Java VM") ;
	}
	
	free(rt->handle) ;
	rt->handle = NULL ;
}
*/

