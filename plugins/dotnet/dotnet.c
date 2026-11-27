/*
 * The .NET plugin, language "dotnet": the CLR through the official hosting
 * API. nethost finds hostfxr, which starts the CLR from
 * Ugly.runtimeconfig.json next to this plugin. The plugin's C# side, Ugly.dll
 * (Bridge.cs, Protocol.cs, Foreign.cs), is loaded into the default load
 * context, so the assemblies it loads later (modules, fixtures) see the same
 * Ugly types; and its [UnmanagedCallersOnly] entry points fill this plugin's
 * vtable, so the core calls C# directly. C# calls the core back through
 * P/Invoke, on the libugly this plugin was linked against.
 *
 * The CLR is one per process and one-way (it can't be unloaded or started
 * again), so there's no 'term', no UGLY_CAP_RESTARTABLE and no
 * UGLY_CAP_MULTI_RUNTIME. It is multi-threaded: UGLY_CAP_ANY_THREAD.
 *
 * Signals: the CLR installs handlers for the faults it turns into exceptions
 * (SIGSEGV, SIGFPE, SIGILL, SIGBUS, SIGTRAP) and for suspending threads
 * (SIGRTMIN), and needs them. For a fault outside managed code they call the
 * handler that was there before, as the JVM's do, so the two VMs coexist in
 * either order. Its other handlers (SIGINT, SIGQUIT, SIGTERM, SIGPIPE,
 * SIGABRT) are for Ctrl-C events, ProcessExit and crash dumps, which belong
 * to the host: init puts back what was there, as -Xrs does for the JVM.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "ugly_plugin.h"

struct hostfxr_dotnet_environment_info ;     /* hostfxr.h declares it inside a parameter list */
#include <nethost.h>
#include <hostfxr.h>
#include <coreclr_delegates.h>

#define BRIDGE_ABI 3     /* Bridge.Abi */


static void diag(const char *what, int rc){
	fprintf(stderr, "ugly dotnet: %s (0x%x)\n", what, (unsigned)rc) ;
}


static ugly_plugin plugin ;     /* filled by dn_init */

/* The host's signals: what the CLR installs for them is undone once it has started. */
static const int host_signals[] = { SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGABRT } ;
#define NHOST (sizeof(host_signals) / sizeof(host_signals[0]))

static void host_handlers(struct sigaction *saved, int restore){
	size_t i ;
	for (i = 0 ; i < NHOST ; i++){
		sigaction(host_signals[i], restore ? &saved[i] : NULL, restore ? NULL : &saved[i]) ;
	}
}

/* The vtable's entries, by the name of their C# entry point in Ugly.Bridge. */
static const struct { const char *method ; void **slot ; } entries[] = {
	{ "RtNew", (void **)&plugin.rt_new },
	{ "RtDelete", (void **)&plugin.rt_delete },
	{ "Import", (void **)&plugin.import },
	{ "Describe", (void **)&plugin.describe },
	{ "GetMember", (void **)&plugin.get_member },
	{ "SetMember", (void **)&plugin.set_member },
	{ "DeleteMember", (void **)&plugin.delete_member },
	{ "MemberNames", (void **)&plugin.member_names },
	{ "Length", (void **)&plugin.length },
	{ "GetIndex", (void **)&plugin.get_index },
	{ "SetIndex", (void **)&plugin.set_index },
	{ "Splice", (void **)&plugin.splice },
	{ "Call", (void **)&plugin.call },
	{ "Invoke", (void **)&plugin.invoke },
	{ "New", (void **)&plugin.new_instance },
	{ "CallOpts", (void **)&plugin.call_opts },
	{ "InvokeOpts", (void **)&plugin.invoke_opts },
	{ "NewOpts", (void **)&plugin.new_opts },
	{ "Iter", (void **)&plugin.iter },
	{ "Next", (void **)&plugin.next },
	{ "ToStr", (void **)&plugin.to_string },
	{ "Release", (void **)&plugin.release },
} ;


/* libugly's dlopen handle, for C#'s P/Invoke: the one this plugin is linked against. */
static void *libugly(void){
	Dl_info info ;
	if (dladdr((void *)ugly_export, &info) == 0){
		return NULL ;
	}
	return dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD) ;     /* never closed, like the CLR */
}


static ugly_status start(const char *dir) ;

static ugly_status dn_init(ugly_ctx *ctx, const char *dir){
	(void)ctx ;
	struct sigaction saved[NHOST] ;
	host_handlers(saved, 0) ;
	ugly_status st = start(dir) ;
	host_handlers(saved, 1) ;
	return st ;
}


/* Starts the CLR, loads Ugly.dll and fills the vtable. */
static ugly_status start(const char *dir){
	char fxr[PATH_MAX], config[PATH_MAX + 32], assembly[PATH_MAX + 16] ;
	size_t size = sizeof fxr ;
	int rc = get_hostfxr_path(fxr, &size, NULL) ;
	if (rc != 0){
		diag("no .NET runtime found (nethost)", rc) ;
		return UGLY_ERR_INTERNAL ;
	}
	void *lib = dlopen(fxr, RTLD_NOW | RTLD_LOCAL) ;     /* never closed: the CLR stays */
	if (lib == NULL){
		fprintf(stderr, "ugly dotnet: %s\n", dlerror()) ;
		return UGLY_ERR_INTERNAL ;
	}
	hostfxr_initialize_for_runtime_config_fn init_fn = (hostfxr_initialize_for_runtime_config_fn)
		dlsym(lib, "hostfxr_initialize_for_runtime_config") ;
	hostfxr_get_runtime_delegate_fn delegate_fn = (hostfxr_get_runtime_delegate_fn)
		dlsym(lib, "hostfxr_get_runtime_delegate") ;
	hostfxr_close_fn close_fn = (hostfxr_close_fn)dlsym(lib, "hostfxr_close") ;
	if ((init_fn == NULL)||(delegate_fn == NULL)||(close_fn == NULL)){
		diag("hostfxr lacks the hosting API", 0) ;
		return UGLY_ERR_INTERNAL ;
	}

	char where[PATH_MAX] ;
	if (realpath(dir, where) == NULL){     /* the CLR loads assemblies by absolute path only */
		fprintf(stderr, "ugly dotnet: %s: %m\n", dir) ;
		return UGLY_ERR_INTERNAL ;
	}
	snprintf(config, sizeof config, "%s/Ugly.runtimeconfig.json", where) ;
	snprintf(assembly, sizeof assembly, "%s/Ugly.dll", where) ;
	hostfxr_handle h = NULL ;
	rc = init_fn(config, NULL, &h) ;     /* < 0: failure; 0, 1, 2: success */
	if ((rc < 0)||(h == NULL)){
		diag("can't start the CLR (hostfxr_initialize_for_runtime_config)", rc) ;
		if (h != NULL){
			close_fn(h) ;
		}
		return UGLY_ERR_INTERNAL ;
	}
	load_assembly_fn load = NULL ;
	get_function_pointer_fn get_fn = NULL ;
	rc = delegate_fn(h, hdt_load_assembly, (void **)&load) ;
	if (rc >= 0){
		rc = delegate_fn(h, hdt_get_function_pointer, (void **)&get_fn) ;
	}
	close_fn(h) ;
	if (rc < 0){
		diag("no load_assembly or get_function_pointer (.NET 8 or later is needed)", rc) ;
		return UGLY_ERR_INTERNAL ;
	}
	rc = load(assembly, NULL, NULL) ;     /* into the default load context */
	if (rc != 0){
		diag("can't load Ugly.dll", rc) ;
		return UGLY_ERR_INTERNAL ;
	}

	int (*bridge_init)(int, void *) = NULL ;
	rc = get_fn("Ugly.Bridge, Ugly", "Init", UNMANAGEDCALLERSONLY_METHOD, NULL, NULL, (void **)&bridge_init) ;
	if (rc != 0){
		diag("no Ugly.Bridge.Init in Ugly.dll", rc) ;
		return UGLY_ERR_INTERNAL ;
	}
	void *core = libugly() ;
	if ((core == NULL)||(bridge_init(BRIDGE_ABI, core) != 0)){
		diag("Ugly.dll doesn't match this plugin", BRIDGE_ABI) ;
		return UGLY_ERR_INTERNAL ;
	}
	size_t i ;
	for (i = 0 ; i < sizeof(entries) / sizeof(entries[0]) ; i++){
		rc = get_fn("Ugly.Bridge, Ugly", entries[i].method, UNMANAGEDCALLERSONLY_METHOD, NULL, NULL, entries[i].slot) ;
		if (rc != 0){
			fprintf(stderr, "ugly dotnet: no Ugly.Bridge.%s in Ugly.dll (0x%x)\n", entries[i].method, (unsigned)rc) ;
			return UGLY_ERR_INTERNAL ;
		}
	}
	return UGLY_OK ;
}


static ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = UGLY_CAP_ANY_THREAD,     /* the CLR is multi-threaded; a thread new to it is attached on entry */
	.name = "dotnet",
	.init = dn_init,
	.options = (const char *const []){ "path", NULL },
} ;


const ugly_plugin *ugly_plugin_dotnet(void){
	return &plugin ;
}
