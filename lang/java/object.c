#include <stdlib.h>
#include <string.h>
#include "ugly_java_priv.h"


ugly_object* _ugly_java_object_new(ugly_context *ctx, ugly_runtime *rt, const char *class, 
	ugly_value **args, int nb_args, const char *hint){
}


ugly_value* _ugly_java_object_call_method(ugly_context *ctx, ugly_object *obj, ugly_type rtype, 
	const char *method, ugly_value **args, int nb_args, const char *hint){
}


void _ugly_java_object_delete(ugly_context *ctx, ugly_object *obj){
}

