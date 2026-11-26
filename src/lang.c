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
static ugly_lang c_lang = { NULL, "c", "", NULL, &ugly_c_plugin, 1, 0, 1, NULL } ;


/*
 * Options (ugly_configure): per language name, process-wide, in the order
 * given. Set before the language loads, read by its plugin. Lock order:
 * langs_lock, then opts_lock; plugins read them in init, under langs_lock.
 */
static int valid_name(const char *name) ;

typedef struct opt {
	char *lang, *key, *value ;
	struct opt *next ;
} opt ;

static pthread_mutex_t opts_lock = PTHREAD_MUTEX_INITIALIZER ;
static opt *opts ;


const char *ugly_option(const char *lang, const char *key, size_t i){
	const char *v = NULL ;
	if ((lang == NULL)||(key == NULL)){
		return NULL ;
	}
	pthread_mutex_lock(&opts_lock) ;
	opt *o ;
	for (o = opts ; o != NULL ; o = o->next){
		if ((strcmp(o->lang, lang) == 0)&&(strcmp(o->key, key) == 0)&&(i-- == 0)){
			v = o->value ;
			break ;
		}
	}
	pthread_mutex_unlock(&opts_lock) ;
	return v ;
}


/* Every option set for this plugin's language must be one it takes. Called under langs_lock. */
static ugly_status check_options(ugly_ctx *ctx, const ugly_plugin *plugin){
	ugly_status st = UGLY_OK ;
	pthread_mutex_lock(&opts_lock) ;
	opt *o ;
	for (o = opts ; (o != NULL)&&(st == UGLY_OK) ; o = o->next){
		if (strcmp(o->lang, plugin->name) != 0){
			continue ;
		}
		int known = 0 ;
		const char *const *k ;
		for (k = plugin->options ; (k != NULL)&&(*k != NULL) ; k++){
			known |= (strcmp(*k, o->key) == 0) ;
		}
		if (! known){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "%s takes no option '%s' (ugly_configure)",
				plugin->name, o->key) ;
		}
	}
	pthread_mutex_unlock(&opts_lock) ;
	return st ;
}


ugly_status ugly_configure(ugly_ctx *ctx, const char *lang, const char *key, const char *value){
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if ((! valid_name(lang))||(key == NULL)||(key[0] == '\0')){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_configure: invalid language or key") ;
	}
	if (strcmp(lang, c_lang.name) == 0){
		return ctx_fail(ctx, UGLY_ERR_IN_USE, "core", "", "the built-in c language takes no options") ;
	}
	pthread_mutex_lock(&langs_lock) ;
	ugly_lang *l ;
	for (l = langs ; l != NULL ; l = l->next){
		if (strcmp(l->name, lang) == 0){
			pthread_mutex_unlock(&langs_lock) ;
			return ctx_fail(ctx, UGLY_ERR_IN_USE, "core", "", "%s has already started: configure it before its first use",
				lang) ;
		}
	}
	pthread_mutex_lock(&opts_lock) ;
	if (value == NULL){
		opt **p = &opts ;
		while (*p != NULL){
			opt *o = *p ;
			if ((strcmp(o->lang, lang) == 0)&&(strcmp(o->key, key) == 0)){
				*p = o->next ;
				free(o->lang) ;
				free(o->key) ;
				free(o->value) ;
				free(o) ;
			}
			else {
				p = &o->next ;
			}
		}
	}
	else {
		opt *o = calloc(1, sizeof(opt)) ;
		if ((o == NULL)||((o->lang = xstrdup(lang)) == NULL)||((o->key = xstrdup(key)) == NULL)
			||((o->value = xstrdup(value)) == NULL)){
			if (o != NULL){
				free(o->lang) ;
				free(o->key) ;
				free(o) ;
			}
			st = ctx_fail(ctx, UGLY_ERR_NOMEM, "core", "", "out of memory") ;
		}
		else {
			opt **p = &opts ;
			while (*p != NULL){
				p = &(*p)->next ;
			}
			*p = o ;
		}
	}
	pthread_mutex_unlock(&opts_lock) ;
	pthread_mutex_unlock(&langs_lock) ;
	return st ;
}


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
			__atomic_add_fetch(&l->loads, 1, __ATOMIC_RELAXED) ;
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

	if ((st = check_options(ctx, plugin)) != UGLY_OK){
		dlclose(dl) ;
		free(dir) ;
		pthread_mutex_unlock(&langs_lock) ;
		return st ;
	}
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
	__atomic_store_n(&l->loads, 1, __ATOMIC_RELAXED) ;
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
		__atomic_sub_fetch(&lang->loads, 1, __ATOMIC_RELAXED) ;
		pthread_mutex_unlock(&langs_lock) ;
		return UGLY_OK ;
	}
	if (__atomic_load_n(&lang->runtimes, __ATOMIC_SEQ_CST) > 0){
		pthread_mutex_unlock(&langs_lock) ;
		return ctx_fail(ctx, UGLY_ERR_IN_USE, "core", "", "can't unload %s: %d runtime(s) still alive",
			lang->name, lang->runtimes) ;
	}
	__atomic_store_n(&lang->loads, 0, __ATOMIC_RELAXED) ;

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
		if (lang->dl != NULL){
			dlclose(lang->dl) ;
		}
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


/* ---------------------------------------------------------------- */
/* Runtimes per language                                            */
/* ---------------------------------------------------------------- */

void lang_add_rt(ugly_lang *lang, ugly_rt *rt){
	pthread_mutex_lock(&langs_lock) ;
	ugly_rt **p = &lang->rts ;          /* appended: the oldest runtime comes first */
	while (*p != NULL){
		p = &(*p)->lang_next ;
	}
	rt->lang_next = NULL ;
	*p = rt ;
	pthread_mutex_unlock(&langs_lock) ;
}


void lang_remove_rt(ugly_lang *lang, ugly_rt *rt){
	pthread_mutex_lock(&langs_lock) ;
	ugly_rt **p ;
	for (p = &lang->rts ; *p != NULL ; p = &(*p)->lang_next){
		if (*p == rt){
			*p = rt->lang_next ;
			break ;
		}
	}
	pthread_mutex_unlock(&langs_lock) ;
}


/* Shares of runtimes, under the langs lock (see ugly_rt in internal.h). */
static int rt_live(ugly_rt *rt){
	return (! __atomic_load_n(&rt->dying, __ATOMIC_SEQ_CST))&&(! __atomic_load_n(&rt->closing, __ATOMIC_SEQ_CST))
		&&(! rt->destroyed) ;
}


ugly_rt *lang_share_rt(ugly_lang *lang){
	pthread_mutex_lock(&langs_lock) ;
	ugly_rt *rt ;
	for (rt = lang->rts ; (rt != NULL)&&(! rt_live(rt)) ; rt = rt->lang_next){
	}
	if (rt != NULL){
		rt->shares++ ;
	}
	pthread_mutex_unlock(&langs_lock) ;
	return rt ;
}


void lang_rt_shares(ugly_rt *rt, int delta, int auto_created){
	pthread_mutex_lock(&langs_lock) ;
	rt->shares += delta ;
	rt->auto_created |= auto_created ;
	pthread_mutex_unlock(&langs_lock) ;
}


int lang_rt_unshare(ugly_rt *rt){
	pthread_mutex_lock(&langs_lock) ;
	int what = RT_KEEP ;
	if (--rt->shares == 0){
		what = (rt->destroyed ? RT_FREE : (rt->auto_created ? RT_DESTROY : RT_KEEP)) ;
	}
	pthread_mutex_unlock(&langs_lock) ;
	return what ;
}


int lang_rt_mark_destroyed(ugly_rt *rt){
	pthread_mutex_lock(&langs_lock) ;
	rt->destroyed = 1 ;
	int free_now = (rt->shares == 0) ;
	pthread_mutex_unlock(&langs_lock) ;
	return free_now ;
}


ugly_rt *lang_find_rt_on_thread(ugly_lang *lang){
	pthread_mutex_lock(&langs_lock) ;
	ugly_rt *rt ;
	for (rt = lang->rts ; rt != NULL ; rt = rt->lang_next){
		if (on_thread(rt->thread)&&rt_live(rt)){
			break ;
		}
	}
	pthread_mutex_unlock(&langs_lock) ;
	return rt ;
}


ugly_lang *lang_find(const char *name){
	if (name == NULL){
		return NULL ;
	}
	pthread_mutex_lock(&langs_lock) ;
	ugly_lang *l ;
	for (l = langs ; l != NULL ; l = l->next){
		if ((l->loads > 0)&&(strcmp(l->name, name) == 0)){
			break ;
		}
	}
	pthread_mutex_unlock(&langs_lock) ;
	return l ;
}


/*
 * A binding registers its language: the plugin is compiled into the binding,
 * which the host interpreter already loaded, so nothing is dlopen()ed (that
 * could bring a second copy of the interpreter into the process).
 */
ugly_status ugly_lang_register(ugly_ctx *ctx, const ugly_plugin *plugin, ugly_lang **out){
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if ((plugin == NULL)||(out == NULL)||(plugin->abi != UGLY_PLUGIN_ABI)||(! valid_name(plugin->name))){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_lang_register: invalid plugin") ;
	}
	*out = NULL ;
	pthread_mutex_lock(&langs_lock) ;
	ugly_lang *l ;
	for (l = langs ; l != NULL ; l = l->next){
		if (strcmp(l->name, plugin->name) == 0){
			if (l->plugin != plugin){
				pthread_mutex_unlock(&langs_lock) ;
				return ctx_fail(ctx, UGLY_ERR_IN_USE, "core", "", "another plugin for %s is already loaded",
					plugin->name) ;
			}
			__atomic_add_fetch(&l->loads, 1, __ATOMIC_RELAXED) ;
			*out = l ;
			pthread_mutex_unlock(&langs_lock) ;
			return UGLY_OK ;
		}
	}
	if ((st = check_options(ctx, plugin)) != UGLY_OK){
		pthread_mutex_unlock(&langs_lock) ;
		return st ;
	}
	if (plugin->init != NULL){
		st = plugin->init(ctx, "") ;
		if (st != UGLY_OK){
			pthread_mutex_unlock(&langs_lock) ;
			if (ctx->error == NULL){
				ctx_fail(ctx, st, plugin->name, "", "plugin initialization failed") ;
			}
			return st ;
		}
	}
	l = calloc(1, sizeof(ugly_lang)) ;
	if (l == NULL){
		pthread_mutex_unlock(&langs_lock) ;
		return ctx_fail(ctx, UGLY_ERR_NOMEM, "core", "", "out of memory") ;
	}
	l->name = xstrdup(plugin->name) ;
	l->dir = xstrdup("") ;
	l->plugin = plugin ;
	__atomic_store_n(&l->loads, 1, __ATOMIC_RELAXED) ;
	l->initialized = 1 ;
	l->next = langs ;
	langs = l ;
	pthread_mutex_unlock(&langs_lock) ;
	*out = l ;
	return UGLY_OK ;
}
