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

	ugly_debug(2, "<- Runtime new") ;
	return rt ;
}


void ugly_runtime_load_library(ugly_context *ctx, ugly_runtime* rt, const char *lib){
	ugly_context_clear_error(ctx) ;

	const ugly_interface *iface = ugly_language_get_iface(ugly_runtime_get_language(rt)) ;
	iface->runtime_load_library(ctx, rt, lib) ;
}


ugly_language *ugly_runtime_get_language(const ugly_runtime *rt){
	return rt->language ;
}


ugly_value *ugly_runtime_call_function(ugly_context *ctx, ugly_runtime *rt, ugly_type rtype, 
	const char *func, ugly_value **args, int nb_args, const char *hint){

	ugly_context_clear_error(ctx) ;

	ugly_debug(2, "-> Runtime call_function: %s", func) ;
	const ugly_interface *iface = ugly_language_get_iface(ugly_runtime_get_language(rt)) ;
	ugly_value *value = iface->runtime_call_function(ctx, rt, rtype, func, args, nb_args, hint) ;
	if (ugly_context_has_error(ctx)){
		return NULL ;
	}

	ugly_debug(2, "<- Runtime call_function") ;
	return value ;
} 
