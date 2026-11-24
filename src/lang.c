/* Loading and unloading language plugins. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"


#ifndef UGLY_PLUGIN_DIR
#define UGLY_PLUGIN_DIR "/usr/local/lib/ugly/plugins"
#endif


static pthread_mutex_t langs_lock = PTHREAD_MUTEX_INITIALIZER ;
static ugly_lang *langs ;

/* The built-in "c" language: home of host callables, lists and maps. Never unloaded. */
static ugly_lang c_lang = { NULL, "c", "", NULL, &ugly_c_plugin, 1, 0, 1 } ;


ugly_lang *lang_builtin_c(void){
	return &c_lang ;
}


static int valid_name(const char *name){
	if ((name == NULL)||(name[0] == '\0')||(strlen(name) > 64)){
		return 0 ;
	}
	const char *p ;
	for (p = name ; *p ; p++){
		if (! (((*p >= 'a')&&(*p <= 'z'))||((*p >= '0')&&(*p <= '9'))||(*p == '_'))){
			return 0 ;
		}
	}
	return 1 ;
}


/* Finds dir/ugly_<name>.so in UGLY_PLUGIN_PATH, then UGLY_PLUGIN_DIR. Returns a malloc'd path. */
static char *find_plugin(const char *name, char **dir_out){
	const char *env = getenv("UGLY_PLUGIN_PATH") ;
	size_t elen = (env != NULL ? strlen(env) : 0) ;
	char *search = malloc(elen + strlen(UGLY_PLUGIN_DIR) + 2) ;
	if (search == NULL){
		return NULL ;
	}
	sprintf(search, "%s%s%s", (env != NULL ? env : ""), (elen > 0 ? ":" : ""), UGLY_PLUGIN_DIR) ;

	char *path = NULL ;
	char *save = NULL ;
	char *d ;
	for (d = strtok_r(search, ":", &save) ; d != NULL ; d = strtok_r(NULL, ":", &save)){
		if (d[0] == '\0'){
			continue ;
		}
		size_t len = strlen(d) + strlen(name) + 16 ;
		char *p = malloc(len) ;
		if (p == NULL){
			break ;
		}
		snprintf(p, len, "%s/ugly_%s.so", d, name) ;
		if (access(p, R_OK) == 0){
			path = p ;
			*dir_out = xstrdup(d) ;
			break ;
		}
		free(p) ;
	}
	free(search) ;
	return path ;
}


ugly_status ugly_lang_load(ugly_ctx *ctx, const char *name, ugly_lang **out){
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if (out == NULL){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_lang_load: out is NULL") ;
	}
	*out = NULL ;
	if (! valid_name(name)){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "invalid language name '%s'", name ? name : "(null)") ;
	}

	pthread_mutex_lock(&langs_lock) ;
	ugly_lang *l ;
	for (l = langs ; l != NULL ; l = l->next){
		if (strcmp(l->name, name) == 0){
			l->loads++ ;
			*out = l ;
			pthread_mutex_unlock(&langs_lock) ;
			return UGLY_OK ;
		}
	}

	char *dir = NULL ;
	char *path = find_plugin(name, &dir) ;
	if (path == NULL){
		pthread_mutex_unlock(&langs_lock) ;
		return ctx_fail(ctx, UGLY_ERR_NOT_FOUND, "core", "", "no plugin ugly_%s.so in UGLY_PLUGIN_PATH or %s",
			name, UGLY_PLUGIN_DIR) ;
	}

	/* RTLD_GLOBAL: modules the embedded interpreter loads (XS, C extensions) need its symbols. */
	void *dl = dlopen(path, RTLD_NOW | RTLD_GLOBAL) ;
	if (dl == NULL){
		st = ctx_fail(ctx, UGLY_ERR_NOT_FOUND, "core", "", "can't load %s: %s", path, dlerror()) ;
		free(path) ;
		free(dir) ;
		pthread_mutex_unlock(&langs_lock) ;
		return st ;
	}

	char sym[96] ;
	snprintf(sym, sizeof(sym), "ugly_plugin_%s", name) ;
	const ugly_plugin *(*entry)(void) = NULL ;
	*(void **)&entry = dlsym(dl, sym) ;
	const ugly_plugin *plugin = (entry != NULL ? entry() : NULL) ;
	if ((plugin == NULL)||(plugin->abi != UGLY_PLUGIN_ABI)){
		st = ctx_fail(ctx, UGLY_ERR_NOT_FOUND, "core", "", "%s: %s", path,
			plugin == NULL ? "no ugly_plugin_<name> entry point" : "plugin ABI mismatch") ;
		dlclose(dl) ;
		free(path) ;
		free(dir) ;
		pthread_mutex_unlock(&langs_lock) ;
		return st ;
	}
	free(path) ;

	if (plugin->init != NULL){
		st = plugin->init(ctx, dir) ;
		if (st != UGLY_OK){
			if (ctx->error == NULL){
				ctx_fail(ctx, st, name, "", "plugin initialization failed") ;
			}
			dlclose(dl) ;
			free(dir) ;
			pthread_mutex_unlock(&langs_lock) ;
			return st ;
		}
	}

	l = calloc(1, sizeof(ugly_lang)) ;
	if (l == NULL){
		if (plugin->term != NULL){
			plugin->term() ;
		}
		dlclose(dl) ;
		free(dir) ;
		pthread_mutex_unlock(&langs_lock) ;
		return ctx_fail(ctx, UGLY_ERR_NOMEM, "core", "", "out of memory") ;
	}
	l->name = xstrdup(name) ;
	l->dir = dir ;
	l->dl = dl ;
	l->plugin = plugin ;
	l->loads = 1 ;
	l->initialized = 1 ;
	l->next = langs ;
	langs = l ;
	pthread_mutex_unlock(&langs_lock) ;

	*out = l ;
	return UGLY_OK ;
}


ugly_status ugly_lang_unload(ugly_ctx *ctx, ugly_lang *lang){
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if (lang == NULL){
		return UGLY_OK ;
	}
	if (lang == &c_lang){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "the built-in c language can't be unloaded") ;
	}

	pthread_mutex_lock(&langs_lock) ;
	if (lang->loads <= 0){
		pthread_mutex_unlock(&langs_lock) ;
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "language %s is not loaded", lang->name) ;
	}
	if (lang->loads > 1){
		/* Another load still holds the language. */
		lang->loads-- ;
		pthread_mutex_unlock(&langs_lock) ;
		return UGLY_OK ;
	}
	if (__atomic_load_n(&lang->runtimes, __ATOMIC_SEQ_CST) > 0){
		pthread_mutex_unlock(&langs_lock) ;
		return ctx_fail(ctx, UGLY_ERR_IN_USE, "core", "", "can't unload %s: %d runtime(s) still alive",
			lang->name, lang->runtimes) ;
	}
	lang->loads = 0 ;

	/*
	 * A restartable plugin is torn down and unmapped. Any other plugin keeps
	 * its process-wide state (it can't be re-created), so it stays mapped and
	 * initialized, and the next load reuses it.
	 */
	if ((lang->plugin->caps & UGLY_CAP_RESTARTABLE)&&(lang->plugin->term != NULL)){
		ugly_lang **p ;
		for (p = &langs ; *p != NULL ; p = &(*p)->next){
			if (*p == lang){
				*p = lang->next ;
				break ;
			}
		}
		lang->plugin->term() ;
		dlclose(lang->dl) ;
		free(lang->name) ;
		free(lang->dir) ;
		free(lang) ;
	}
	pthread_mutex_unlock(&langs_lock) ;
	return UGLY_OK ;
}


const char *ugly_lang_name(const ugly_lang *lang){
	return lang->name ;
}


uint32_t ugly_lang_caps(const ugly_lang *lang){
	return lang->plugin->caps ;
}
