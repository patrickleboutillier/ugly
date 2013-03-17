#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>


#include "ugly_priv.h"


ugly_language *ugly_language_init(ugly_context *ctx, const char *name){
	ugly_debug(2, "-> Language init: %s", name) ;
	ugly_context_clear_error(ctx) ;
	dlerror() ; 

	char *buf = (char *)malloc((strlen(name) + 32) * sizeof(char)) ;
	sprintf(buf, "libugly_%s.so", name) ;
	void *handle = dlopen(buf, RTLD_LAZY | RTLD_GLOBAL) ;
	if (handle == NULL){
		ugly_context_set_error(ctx, UGLY_INTERNAL_ERROR, "Can't open shared object %s: %s\n", buf, dlerror()) ;
		free(buf) ;
		return NULL ;
	}
	free(buf) ;

	/* Now that the plugin is loaded, we must initialize it */
	buf = (char *)malloc((strlen(name) + 32) * sizeof(char)) ;
	sprintf(buf, "_ugly_%s_language_init", name) ;

	dlerror() ; 
	ugly_interface *(*init)(ugly_context *ctx, const char *name) = NULL ;
	init = dlsym(handle, buf) ;
	const char *error ;
	if ((error = dlerror()) != NULL){
		ugly_context_set_error(ctx, UGLY_INTERNAL_ERROR, "Can't find symbol %s: %s\n", buf, error) ;
		free(buf) ;
		return NULL ;
	}
	free(buf) ;

	/* Build the dir name where the plugin helper files can be. */
	char *dir = (char *)malloc((strlen(UGLY_LANGUAGE_DIR) + strlen(name) + 32) * sizeof(char)) ;
	sprintf(dir, "%s/%s", UGLY_LANGUAGE_DIR, name) ;
	ugly_interface *iface = (*init)(ctx, name) ;
	if (ugly_context_has_error(ctx)){
		free(dir) ;
		return NULL ;
	}

	ugly_language *lang = UGLY_MALLOC(ugly_language) ;
	lang->name = strdup(name) ;
	lang->dir = dir ;
	lang->handle = handle ;
	lang->iface = iface ;

	ugly_debug(2, "<- Language init") ;
	return lang ;
}


const char *ugly_language_get_name(const ugly_language *lang){
	return lang->name ;
}


const char *ugly_language_get_dir(const ugly_language *lang){
	return lang->dir ;
}


const ugly_interface *ugly_language_get_iface(const ugly_language *lang){
	return lang->iface ;
}
