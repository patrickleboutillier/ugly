#include <stdlib.h>
#include <string.h>
#include <stdio.h>


#include "ugly_priv.h"


ugly_object *ugly_object_alloc(ugly_runtime* rt, const char *class, void *handle){
	ugly_object *o = UGLY_MALLOC(ugly_object) ;
	o->runtime = rt ;
	o->class = strdup(class) ;
	o->handle = handle ;
	o->stringy = NULL ;

	return o ;
}


const char *ugly_object_get_class(const ugly_object *obj){
	return obj->class ;
}


void *ugly_object_get_handle(const ugly_object *obj){
	return obj->handle ;
}


ugly_runtime *ugly_object_get_runtime(const ugly_object *obj){
    return obj->runtime ;
}


const char *ugly_object_to_string(ugly_object *obj){
	if (obj->stringy != NULL){
        return obj->stringy ;
    }

	const char *lang = ugly_language_get_name(ugly_runtime_get_language(ugly_object_get_runtime(obj))) ;
	int len = strlen(obj->class) + strlen(lang) + 64 ;
	obj->stringy = (char *)malloc(len * sizeof(char)) ;
	if (obj->handle != NULL){
		snprintf(obj->stringy, len-1, "%s::%s(%p)", lang, obj->class, obj->handle) ;
	}
	else {
		snprintf(obj->stringy, len-1, "%s::%s(NULL)", lang, obj->class) ;
	}

	return obj->stringy ;
}


ugly_object *ugly_object_new(ugly_context *ctx, ugly_runtime* rt, const char *class, ugly_value **args, int nb_args, const char *hint){
	ugly_context_clear_error(ctx) ;

	ugly_debug(2, "-> Object new: %s::%s(%s)", ugly_runtime_to_string(rt), class, ugly_debug_args(args, nb_args)) ;
	const ugly_interface *iface = ugly_language_get_iface(rt->language) ;
	ugly_object *ret = iface->object_new(ctx, rt, class, args, nb_args, hint) ;
    if (ugly_context_has_error(ctx)){
        return NULL ;
    }

	ugly_debug(2, "<- Object new") ;
    return ret ;
}


ugly_value *ugly_object_call_method(ugly_context *ctx, ugly_object *obj, ugly_type rtype, const char *method, ugly_value **args, int nb_args, const char *hint){
	ugly_context_clear_error(ctx) ;

	ugly_debug(2, "-> Object call_method: (%s)%s::%s(%s)", ugly_type_to_string(rtype), 
		ugly_object_to_string(obj), method, ugly_debug_args(args, nb_args)) ;

	const ugly_interface *iface = ugly_language_get_iface(obj->runtime->language) ;
	ugly_value *ret = iface->object_call_method(ctx, obj, rtype, method, args, nb_args, hint) ;
    if (ugly_context_has_error(ctx)){
        return NULL ;
    }

	ugly_debug(2, "<- Object call_method") ;
    return ret ;
}


void ugly_object_delete(ugly_context *ctx, ugly_object *obj){
	ugly_context_clear_error(ctx) ;

	ugly_debug(2, "-> Object delete: %s", ugly_object_to_string(obj)) ;
	const ugly_interface *iface = ugly_language_get_iface(obj->runtime->language) ;
	iface->object_delete(ctx, obj) ;

	free(obj->class) ;
	if (obj->stringy != NULL){
		free(obj->stringy) ;
	}
	free(obj) ;
	ugly_debug(2, "<- Object delete") ;
}
