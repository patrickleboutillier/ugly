#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>


#include "ugly_priv.h"


ugly_runtime *ugly_runtime_new(ugly_context *ctx, ugly_language *lang){
	ugly_context_clear_error(ctx) ;

	ugly_debug(2, "-> Runtime new: %s", ugly_language_get_name(lang)) ;
	const ugly_interface *iface = ugly_language_get_iface(lang) ;
	void *runtime = iface->runtime_new(ctx, lang) ;
	if (ugly_context_has_error(ctx)){
		return NULL ;
	}

	ugly_runtime *rt = UGLY_MALLOC(ugly_runtime) ;
	rt->language = lang ;
	rt->handle = runtime ;
	rt->nb_objects = 0 ;
	lang->nb_runtimes++ ;

	ugly_debug(2, "<- Runtime new") ;
	return rt ;
}


void ugly_runtime_load_library(ugly_context *ctx, ugly_runtime* rt, const char *lib){
	ugly_context_clear_error(ctx) ;

	ugly_debug(2, "-> Runtime load_library: %s::%s", ugly_runtime_to_string(rt), lib) ;
	const ugly_interface *iface = ugly_language_get_iface(ugly_runtime_get_language(rt)) ;
	iface->runtime_load_library(ctx, rt, lib) ;
	ugly_debug(2, "<- Runtime load_library") ;
}


/*
 * Refused (UGLY_IN_USE) while objects of this runtime are alive: their
 * handles point into it. On refusal or plugin error the runtime is left intact.
*/
void ugly_runtime_delete(ugly_context *ctx, ugly_runtime *rt){
	ugly_context_clear_error(ctx) ;
	if (rt == NULL){
		return ;
	}

	ugly_debug(2, "-> Runtime delete: %s", ugly_runtime_to_string(rt)) ;
	if (rt->nb_objects > 0){
		ugly_context_set_error(ctx, UGLY_IN_USE, "Can't delete runtime %s: %d object(s) still alive",
			ugly_runtime_to_string(rt), rt->nb_objects) ;
		return ;
	}

	const ugly_interface *iface = ugly_language_get_iface(rt->language) ;
	iface->runtime_delete(ctx, rt) ;
	if (ugly_context_has_error(ctx)){
		return ;
	}

	rt->language->nb_runtimes-- ;
	free(rt) ;
	ugly_debug(2, "<- Runtime delete") ;
}


ugly_language *ugly_runtime_get_language(const ugly_runtime *rt){
	return rt->language ;
}


const char *ugly_runtime_to_string(const ugly_runtime *rt){
	static char s[128] ;

	sprintf(s, "%s::rt(%p)", ugly_language_get_name(ugly_runtime_get_language(rt)), rt) ;

	return s ;
}



ugly_value *ugly_runtime_call_function(ugly_context *ctx, ugly_runtime *rt, ugly_type rtype, 
	const char *func, ugly_value **args, int nb_args, const char *hint){

	ugly_context_clear_error(ctx) ;

	ugly_debug(2, "-> Runtime call_function: (%s)%s::%s(%s)", ugly_type_to_string(rtype), 
		ugly_runtime_to_string(rt), func, ugly_debug_args(args, nb_args)) ;
	const ugly_interface *iface = ugly_language_get_iface(ugly_runtime_get_language(rt)) ;
	ugly_value *value = iface->runtime_call_function(ctx, rt, rtype, func, args, nb_args, hint) ;
	if (ugly_context_has_error(ctx)){
		return NULL ;
	}

	ugly_debug(2, "<- Runtime call_function") ;
	return value ;
} 
