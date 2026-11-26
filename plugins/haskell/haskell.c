/*
 * The Haskell plugin, language "haskell": GHC's runtime system, started in
 * the host's process. The Haskell side (the modules in Ugly/) is
 * libugly_hs.so, and rts.c is libugly_rts.so, both built by ghc and linked
 * against GHC's shared libraries and threaded RTS; this file is the vtable,
 * reaching them through their foreign exports.
 *
 * init starts the RTS and checks that the two sides agree; runtimes come and
 * go, from any thread. import finds a module in a module library (below).
 * The rest of the protocol is Haskell's (Ugly.Plugin), on boxes: describe,
 * members, elements, call and invoke (receiver last). Haskell values are
 * immutable, so writing is refused. The RTS is one-way (it can't be started
 * again once stopped), so there's no 'term' and no UGLY_CAP_RESTARTABLE.
 * doc/haskell-bridge.md is the plan.
 */
#define _GNU_SOURCE     /* RTLD_DEFAULT */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ugly_plugin.h"

#define BRIDGE_ABI 4     /* Ugly.Plugin.bridgeAbi */

/* libugly_rts.so */
extern int ugly_hs_start(const char *opts) ;
/* libugly_hs.so: Ugly.Plugin's foreign exports. */
extern int ugly_hs_abi(void) ;
extern void *ugly_hs_rt_new(void) ;              /* StablePtr Runtime */
extern void ugly_hs_rt_delete(void *rt) ;
/* Ugly.Plugin's entry points, with the vtable's signatures. */
extern ugly_status ugly_hs_import(ugly_ctx *ctx, ugly_rt *rt, void *root, ugly_val *out) ;
extern ugly_status ugly_hs_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name) ;
extern ugly_status ugly_hs_get_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want,
	ugly_val *out) ;
extern ugly_status ugly_hs_member_names(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out) ;
extern ugly_status ugly_hs_length(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out) ;
extern ugly_status ugly_hs_get_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want,
	ugly_val *out) ;
extern ugly_status ugly_hs_call(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out) ;
extern ugly_status ugly_hs_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out) ;
extern void ugly_hs_release(ugly_rt *rt, void *ref) ;


/*
 * The "rts" option: more RTS options (-N4, -M1g), each value one or more
 * words, as between +RTS and -RTS. The RTS also reads GHCRTS.
 */
static char *rts_options(void){
	size_t size = 1 ;
	const char *v ;
	for (size_t i = 0 ; (v = ugly_option("haskell", "rts", i)) != NULL ; i++){
		size += strlen(v) + 1 ;
	}
	char *opts = malloc(size) ;
	if (opts == NULL){
		return NULL ;
	}
	opts[0] = '\0' ;
	for (size_t i = 0 ; (v = ugly_option("haskell", "rts", i)) != NULL ; i++){
		if (i > 0){
			strcat(opts, " ") ;
		}
		strcat(opts, v) ;
	}
	return opts ;
}


static ugly_status hs_init(ugly_ctx *ctx, const char *dir){
	(void)ctx ;
	(void)dir ;
	char *opts = rts_options() ;
	if ((opts == NULL)||(ugly_hs_start(opts) != 0)){
		free(opts) ;
		return UGLY_ERR_NOMEM ;
	}
	free(opts) ;
	if (ugly_hs_abi() != BRIDGE_ABI){
		fprintf(stderr, "ugly haskell: libugly_hs.so doesn't match this plugin (%d)\n", BRIDGE_ABI) ;
		return UGLY_ERR_INTERNAL ;
	}
	return UGLY_OK ;
}


static ugly_status hs_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	(void)ctx ;
	(void)rt ;
	*native = ugly_hs_rt_new() ;
	return UGLY_OK ;
}


static ugly_status hs_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	(void)ctx ;
	(void)rt ;
	ugly_hs_rt_delete(native) ;
	return UGLY_OK ;
}


/*
 * Modules. A module library is a .so built against the package 'ugly' (no
 * RTS: it joins this one), holding one or more modules, each a foreign
 * export named after it (Ugly.Export.moduleSymbol) that gives a new stable
 * pointer to the module's box. import "A.B.C" looks for that symbol in the
 * process (the host program's own, or a library already loaded globally),
 * then in <dir>/A.B.C.so, <dir>/A.B.so, <dir>/A.so (the longest prefix
 * first) for each dir of the "path" option, then of UGLY_HASKELL_PATH
 * (colon-separated).
 *
 * Libraries are opened RTLD_LOCAL, so that two of them can hold Haskell
 * modules of the same name, and never closed: Haskell code can't be unloaded
 * (the RTS holds pointers into it). So modules are per process, found once
 * and cached; every runtime gets the same box.
 */
typedef void *(*module_root)(void) ;      /* IO (StablePtr Box) */

struct module {
	char *name ;
	module_root root ;
	struct module *next ;
} ;
static struct module *modules ;
static pthread_mutex_t modules_lock = PTHREAD_MUTEX_INITIALIZER ;

/* ugly_hs_module_, then name's bytes: ASCII letters and digits as they are, any other as _ and two hex digits. */
static char *module_symbol(const char *name){
	static const char prefix[] = "ugly_hs_module_" ;
	char *sym = malloc(sizeof prefix + 3 * strlen(name)), *p ;
	if (sym == NULL){
		return NULL ;
	}
	p = sym + sprintf(sym, "%s", prefix) ;
	for (const unsigned char *c = (const unsigned char *)name ; *c != '\0' ; c++){
		if (((*c >= 'a')&&(*c <= 'z'))||((*c >= 'A')&&(*c <= 'Z'))||((*c >= '0')&&(*c <= '9'))){
			*p++ = (char)*c ;
		}
		else {
			p += sprintf(p, "_%02x", *c) ;
		}
	}
	*p = '\0' ;
	return sym ;
}

/* The module path: the "path" option's values, then UGLY_HASKELL_PATH, joined by colons. */
static char *module_path(void){
	const char *env = getenv("UGLY_HASKELL_PATH"), *v ;
	size_t len = (env != NULL ? strlen(env) : 0) + 1 ;
	for (size_t i = 0 ; (v = ugly_option("haskell", "path", i)) != NULL ; i++){
		len += strlen(v) + 1 ;
	}
	char *search = malloc(len) ;
	if (search == NULL){
		return NULL ;
	}
	search[0] = '\0' ;
	for (size_t i = 0 ; (v = ugly_option("haskell", "path", i)) != NULL ; i++){
		strcat(search, v) ;
		strcat(search, ":") ;
	}
	strcat(search, env != NULL ? env : "") ;
	return search ;
}

/*
 * Looks for module 'name' on the path, under modules_lock: its root, or NULL
 * with 'why' set if a library that would have it can't be loaded.
 */
static module_root search_path(const char *name, const char *sym, char *why, size_t size){
	char *search = module_path() ;
	char *prefix = strdup(name) ;
	module_root root = NULL ;
	why[0] = '\0' ;
	if ((search == NULL)||(prefix == NULL)){
		snprintf(why, size, "out of memory") ;
	}
	/* The longest prefix first: A.B.C, then A.B, then A. */
	for (char *dot = NULL ; (prefix != NULL)&&(root == NULL)&&(why[0] == '\0') ; *dot = '\0'){
		char *dirs = strdup(search), *save = NULL ;
		for (char *dir = (dirs != NULL ? strtok_r(dirs, ":", &save) : NULL) ; (dir != NULL)&&(root == NULL) ;
			dir = strtok_r(NULL, ":", &save)){
			char path[4096] ;
			if ((size_t)snprintf(path, sizeof path, "%s/%s.so", dir, prefix) >= sizeof path){
				continue ;
			}
			if (access(path, F_OK) != 0){
				continue ;
			}
			void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL) ;
			if (lib == NULL){
				snprintf(why, size, "%s", dlerror()) ;
				break ;
			}
			root = (module_root)dlsym(lib, sym) ;     /* not there: the library stays, as one can't be unloaded */
		}
		free(dirs) ;
		if ((dot = strrchr(prefix, '.')) == NULL){
			break ;
		}
	}
	free(prefix) ;
	free(search) ;
	return root ;
}

static ugly_status hs_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out){
	module_root root = NULL ;
	char why[512] = "" ;
	char *sym = module_symbol(name) ;
	if (sym == NULL){
		return UGLY_ERR_NOMEM ;
	}
	pthread_mutex_lock(&modules_lock) ;
	for (struct module *m = modules ; (m != NULL)&&(root == NULL) ; m = m->next){
		if (strcmp(m->name, name) == 0){
			root = m->root ;
		}
	}
	if (root == NULL){
		root = (module_root)dlsym(RTLD_DEFAULT, sym) ;
	}
	/* A name with a slash isn't a module's, and won't become a path. */
	if ((root == NULL)&&(strchr(name, '/') == NULL)&&(name[0] != '\0')){
		root = search_path(name, sym, why, sizeof why) ;
		struct module *m = (root != NULL ? malloc(sizeof *m) : NULL) ;
		if ((m != NULL)&&((m->name = strdup(name)) != NULL)){
			m->root = root ;
			m->next = modules ;
			modules = m ;
		}
		else {
			free(m) ;
		}
	}
	pthread_mutex_unlock(&modules_lock) ;
	free(sym) ;

	if (root == NULL){
		char msg[1024] ;
		if (why[0] != '\0'){
			snprintf(msg, sizeof msg, "Could not load module \"%s\": %s", name, why) ;
		}
		else {
			snprintf(msg, sizeof msg, "Could not find module \"%s\" (no library on the \"path\" option or "
				"UGLY_HASKELL_PATH has it)", name) ;
		}
		return ugly_fail(ctx, rt, UGLY_ERR_NOT_FOUND, "ModuleNotFound", msg) ;
	}
	return ugly_hs_import(ctx, rt, root(), out) ;
}


/* Haskell values are immutable. */
#define IMMUTABLE() return ugly_fail(ctx, rt, UGLY_ERR_NOT_CAPABLE, "TypeError", "Haskell values are immutable")

/* Not yet. */
#define TODO(op) return ugly_fail(ctx, rt, UGLY_ERR_NOT_IMPLEMENTED, "", "haskell: " op " isn't implemented yet")

static ugly_status hs_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_val *out){
	TODO("globals") ;
}
static ugly_status hs_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	IMMUTABLE() ;
}
static ugly_status hs_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	IMMUTABLE() ;
}
static ugly_status hs_new_instance(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_handle *out){
	TODO("new_instance") ;
}
static ugly_status hs_delete_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name){
	IMMUTABLE() ;
}
static ugly_status hs_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n){
	IMMUTABLE() ;
}


static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = UGLY_CAP_MULTI_RUNTIME | UGLY_CAP_ANY_THREAD,
	.name = "haskell",
	.init = hs_init,
	.rt_new = hs_rt_new,
	.rt_delete = hs_rt_delete,
	.import = hs_import,
	.globals = hs_globals,
	.describe = ugly_hs_describe,
	.get_member = ugly_hs_get_member,
	.set_member = hs_set_member,
	.member_names = ugly_hs_member_names,
	.length = ugly_hs_length,
	.get_index = ugly_hs_get_index,
	.set_index = hs_set_index,
	.call = ugly_hs_call,
	.invoke = ugly_hs_invoke,
	.new_instance = hs_new_instance,
	.delete_member = hs_delete_member,
	.splice = hs_splice,
	.release = ugly_hs_release,
	.options = (const char *const []){ "rts", "path", NULL },
} ;


const ugly_plugin *ugly_plugin_haskell(void){
	return &plugin ;
}
