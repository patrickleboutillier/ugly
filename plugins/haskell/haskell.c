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
 * members, elements, call and invoke (receiver last), new (a constructor).
 * Haskell values are immutable, so writing is refused, except for the
 * mutable values a module makes itself (IORefs behind a box). The RTS is one-way (it can't be started
 * again once stopped), so there's no 'term' and no UGLY_CAP_RESTARTABLE.
 * doc/languages/haskell.md is the guide.
 */
#define _GNU_SOURCE     /* RTLD_DEFAULT */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ugly_plugin.h"

#define BRIDGE_ABI 7     /* Ugly.Plugin.bridgeAbi */

#ifndef UGLY_BINDING
/* libugly_rts.so */
extern int ugly_hs_start(const char *opts) ;
#else
/* The program's RTS (binding.c): keep the CAFs of module libraries loaded later. */
extern void setKeepCAFs(void) ;
#endif
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
/* call and invoke take the hint call option, or NULL. */
extern ugly_status ugly_hs_call(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const char *hint, ugly_kind want, ugly_val *out) ;
extern ugly_status ugly_hs_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, const char *hint, ugly_kind want, ugly_val *out) ;
extern void ugly_hs_release(ugly_rt *rt, void *ref) ;
/* Iteration (lists, lazily; boxes that declare it) and show. */
extern ugly_status ugly_hs_iter(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out) ;
extern ugly_status ugly_hs_next(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_kind want, ugly_val *out, size_t max,
	size_t *got) ;
extern ugly_status ugly_hs_to_string(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_val *out) ;
/* Mutable values (an IORef behind a box), and constructors. */
extern ugly_status ugly_hs_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, const ugly_val *v) ;
extern ugly_status ugly_hs_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, const ugly_val *v) ;
extern ugly_status ugly_hs_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n) ;
extern ugly_status ugly_hs_new_instance(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_handle *out) ;


#ifndef UGLY_BINDING
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
#else
/*
 * The binding (binding.c): the RTS is the program's own, already running.
 * It wasn't started with keep_cafs, which module libraries loaded later need
 * (rts.c): setKeepCAFs does the same for the CAFs entered from now on.
 */
static ugly_status hs_init(ugly_ctx *ctx, const char *dir){
	(void)ctx ;
	(void)dir ;
	setKeepCAFs() ;
	return UGLY_OK ;
}

/* The program's own runtime: plain Haskell state, as for rt_new. */
static ugly_status hs_rt_attach(ugly_ctx *ctx, ugly_rt *rt, void *existing, void **native){
	(void)ctx ;
	(void)rt ;
	(void)existing ;
	*native = ugly_hs_rt_new() ;
	return UGLY_OK ;
}
#endif


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


/*
 * Calls. Call options (contract §11): the hint is type arguments, as
 * TypeApplications gives them ("Chord", "@Scale @Note", "@_ @Note"),
 * choosing among a polymorphic function's instantiations (Ugly.Value's
 * overloaded). Haskell has no keyword arguments and no call context: both
 * are refused.
 */

/*
 * The C stack. Each call into Haskell from C runs the RTS's scheduler on the
 * caller's stack, and StgRun reserves 16 KB more: tens of KB a level, so
 * re-entry (C -> Haskell -> C -> ...) exhausts an 8 MB stack long before
 * the core's UGLY_MAX_DEPTH. A call is refused, as Haskell's StackOverflow,
 * once less than STACK_RESERVE is left (other languages have the same
 * limit: Lua's C calls, QuickJS's stack check, .NET's
 * EnsureSufficientExecutionStack).
 */
#define STACK_RESERVE (1024 * 1024)

static _Thread_local const char *stack_floor ;     /* lowest address a call may start at; (char *)1 if unknown */

static int stack_left(void){
	if (stack_floor == NULL){
		pthread_attr_t attr ;
		void *low = NULL ;
		size_t size = 0 ;
		stack_floor = (const char *)1 ;
		if (pthread_getattr_np(pthread_self(), &attr) == 0){
			if ((pthread_attr_getstack(&attr, &low, &size) == 0)&&(size > 2 * STACK_RESERVE)){
				stack_floor = (const char *)low + STACK_RESERVE ;
			}
			pthread_attr_destroy(&attr) ;
		}
	}
	char here ;
	return &here > stack_floor ;
}

#define STACK_CHECK() \
	if (! stack_left()){ \
		return ugly_fail(ctx, rt, UGLY_ERR_RAISED, "StackOverflow", \
			"haskell: the C stack is nearly exhausted (calls nested too deep through Haskell)") ; \
	}

static ugly_status hs_call(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs, ugly_kind want,
	ugly_val *out){
	STACK_CHECK() ;
	return ugly_hs_call(ctx, rt, ref, args, nargs, NULL, want, out) ;
}

static ugly_status hs_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out){
	STACK_CHECK() ;
	return ugly_hs_invoke(ctx, rt, ref, method, args, nargs, NULL, want, out) ;
}

/* The options Haskell can't honour, refused. */
static ugly_status refuse_opts(ugly_ctx *ctx, ugly_rt *rt, const ugly_opts *opts){
	if (opts->nkwargs > 0){
		return ugly_fail(ctx, rt, UGLY_ERR_NOT_SUPPORTED, "TypeError",
			"haskell: keyword arguments aren't supported (Haskell has none)") ;
	}
	if (opts->context != NULL){
		return ugly_fail(ctx, rt, UGLY_ERR_NOT_SUPPORTED, "TypeError",
			"haskell: a call context isn't supported (Haskell has none)") ;
	}
	return UGLY_OK ;
}

static ugly_status hs_call_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *opts, ugly_kind want, ugly_val *out){
	STACK_CHECK() ;
	ugly_status st = refuse_opts(ctx, rt, opts) ;
	return st != UGLY_OK ? st : ugly_hs_call(ctx, rt, ref, args, nargs, opts->hint, want, out) ;
}

static ugly_status hs_invoke_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, const ugly_opts *opts, ugly_kind want, ugly_val *out){
	STACK_CHECK() ;
	ugly_status st = refuse_opts(ctx, rt, opts) ;
	return st != UGLY_OK ? st : ugly_hs_invoke(ctx, rt, ref, method, args, nargs, opts->hint, want, out) ;
}


/* Members are fixed: Haskell's records, and the mutable values a module makes. */
#define FIXED() return ugly_fail(ctx, rt, UGLY_ERR_NOT_CAPABLE, "TypeError", "a Haskell value's members are fixed")

/* Haskell has no global namespace: a module's exports are its members. */
static ugly_status hs_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_val *out){
	(void)out ;
	return ugly_fail(ctx, rt, UGLY_ERR_NOT_SUPPORTED, "", "haskell: no globals (Haskell has none: import a module)") ;
}

/* A value passed by value, to Haskell by pointer. */
static ugly_status hs_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	return ugly_hs_set_member(ctx, rt, ref, name, &v) ;
}
static ugly_status hs_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	return ugly_hs_set_index(ctx, rt, ref, i, &v) ;
}
static ugly_status hs_delete_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name){
	(void)ref ;
	(void)name ;
	FIXED() ;
}


static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
#ifndef UGLY_BINDING
	.caps = UGLY_CAP_MULTI_RUNTIME | UGLY_CAP_ANY_THREAD,
#else
	.caps = UGLY_CAP_MULTI_RUNTIME | UGLY_CAP_ANY_THREAD | UGLY_CAP_ATTACH,
	.rt_attach = hs_rt_attach,
#endif
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
	.call = hs_call,
	.invoke = hs_invoke,
	.new_instance = ugly_hs_new_instance,
	.delete_member = hs_delete_member,
	.splice = ugly_hs_splice,
	.release = ugly_hs_release,
#ifndef UGLY_BINDING
	.options = (const char *const []){ "rts", "path", NULL },
#else
	.options = (const char *const []){ "path", NULL },      /* the RTS's options are the program's */
#endif
	.call_opts = hs_call_opts,
	.invoke_opts = hs_invoke_opts,
	.iter = ugly_hs_iter,
	.next = ugly_hs_next,
	.to_string = ugly_hs_to_string,
} ;


#ifndef UGLY_BINDING
const ugly_plugin *ugly_plugin_haskell(void){
	return &plugin ;
}
#endif
