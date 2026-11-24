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
	ugly_interface *(*init)(ugly_context *ctx, const char *name, const char *dir) = NULL ;
	init = dlsym(handle, buf) ;
	const char *error ;
	if ((error = dlerror()) != NULL){
		ugly_context_set_error(ctx, UGLY_INTERNAL_ERROR, "Can't find symbol %s: %s\n", buf, error) ;
		free(buf) ;
		dlclose(handle) ;
		return NULL ;
	}
	free(buf) ;

	/* Build the dir name where the plugin helper files can be. */
	const char *base = getenv("UGLY_LANGUAGE_DIR") ;
	if ((base == NULL)||(base[0] == '\0')){
		base = UGLY_LANGUAGE_DIR ;
	}
	char *dir = (char *)malloc((strlen(base) + strlen(name) + 32) * sizeof(char)) ;
	sprintf(dir, "%s/%s", base, name) ;
	ugly_interface *iface = (*init)(ctx, name, dir) ;
	if (ugly_context_has_error(ctx)){
		free(dir) ;
		dlclose(handle) ;
		return NULL ;
	}

	ugly_language *lang = UGLY_MALLOC(ugly_language) ;
	lang->name = strdup(name) ;
	lang->dir = dir ;
	lang->handle = handle ;
	lang->nb_runtimes = 0 ;
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


/*
 * Refused (UGLY_IN_USE) while runtimes of this language are alive: the
 * plugin's language_delete may tear down process-wide state that they need.
 * On refusal or plugin error the language is left intact.
*/
void ugly_language_delete(ugly_context *ctx, ugly_language *lang){
	ugly_context_clear_error(ctx) ;
	if (lang == NULL){
		return ;
	}

	ugly_debug(2, "-> Language delete: %s", lang->name) ;
	if (lang->nb_runtimes > 0){
		ugly_context_set_error(ctx, UGLY_IN_USE, "Can't delete language %s: %d runtime(s) still alive",
			lang->name, lang->nb_runtimes) ;
		return ;
	}

	lang->iface->language_delete(ctx, lang) ;
	if (ugly_context_has_error(ctx)){
		return ;
	}

	free(lang->iface) ;
	dlclose(lang->handle) ;
	free(lang->name) ;
	free(lang->dir) ;
	free(lang) ;
	ugly_debug(2, "<- Language delete") ;
}
