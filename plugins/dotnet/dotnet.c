/*
 * The .NET plugin, language "dotnet": the CLR through the official hosting
 * API. nethost finds hostfxr, which starts the CLR from Ugly.runtimeconfig.json
 * next to this plugin; the C# side (Ugly.dll, Bridge.cs) is reached through
 * [UnmanagedCallersOnly] entry points.
 *
 * Skeleton: init starts the CLR and checks that the two sides agree; runtimes
 * can be created and deleted; every operation is refused. The CLR is one-way
 * (it can't be unloaded or started again), so there's no 'term' and no
 * UGLY_CAP_RESTARTABLE.
 */
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>

#include "ugly_plugin.h"

struct hostfxr_dotnet_environment_info ;     /* hostfxr.h declares it inside a parameter list */
#include <nethost.h>
#include <hostfxr.h>
#include <coreclr_delegates.h>

#define BRIDGE_ABI 1     /* Bridge.Abi */

static load_assembly_and_get_function_pointer_fn load_fn ;
static char assembly[PATH_MAX] ;


static void diag(const char *what, int rc){
	fprintf(stderr, "ugly dotnet: %s (0x%x)\n", what, (unsigned)rc) ;
}


/* An [UnmanagedCallersOnly] method of Ugly.Bridge. */
static int bridge_fn(const char *method, void **out){
	return load_fn(assembly, "Ugly.Bridge, Ugly", method, UNMANAGEDCALLERSONLY_METHOD, NULL, out) ;
}


static ugly_status dn_init(ugly_ctx *ctx, const char *dir){
	(void)ctx ;
	char fxr[PATH_MAX], config[PATH_MAX] ;
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

	snprintf(config, sizeof config, "%s/Ugly.runtimeconfig.json", dir) ;
	snprintf(assembly, sizeof assembly, "%s/Ugly.dll", dir) ;
	hostfxr_handle h = NULL ;
	rc = init_fn(config, NULL, &h) ;     /* < 0: failure; 0, 1, 2: success */
	if ((rc < 0)||(h == NULL)){
		diag("can't start the CLR (hostfxr_initialize_for_runtime_config)", rc) ;
		if (h != NULL){
			close_fn(h) ;
		}
		return UGLY_ERR_INTERNAL ;
	}
	rc = delegate_fn(h, hdt_load_assembly_and_get_function_pointer, (void **)&load_fn) ;
	close_fn(h) ;
	if (rc < 0){
		diag("no load_assembly_and_get_function_pointer", rc) ;
		return UGLY_ERR_INTERNAL ;
	}

	int (*bridge_init)(int) = NULL ;
	rc = bridge_fn("Init", (void **)&bridge_init) ;
	if (rc != 0){
		diag("can't load Ugly.Bridge.Init from Ugly.dll", rc) ;
		return UGLY_ERR_INTERNAL ;
	}
	if (bridge_init(BRIDGE_ABI) != 0){
		diag("Ugly.dll doesn't match this plugin", BRIDGE_ABI) ;
		return UGLY_ERR_INTERNAL ;
	}
	return UGLY_OK ;
}


static ugly_status dn_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	*native = NULL ;
	return UGLY_OK ;
}


static ugly_status dn_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	return UGLY_OK ;
}


/* Not yet: every operation is refused. */
#define TODO(op) return ugly_fail(ctx, rt, UGLY_ERR_NOT_SUPPORTED, "", "dotnet: " op " isn't implemented yet")

static ugly_status dn_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out){
	TODO("import") ;
}
static ugly_status dn_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_val *out){
	TODO("globals") ;
}
static ugly_status dn_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name){
	TODO("describe") ;
}
static ugly_status dn_get_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want,
	ugly_val *out){
	TODO("get_member") ;
}
static ugly_status dn_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	TODO("set_member") ;
}
static ugly_status dn_member_names(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	TODO("member_names") ;
}
static ugly_status dn_length(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out){
	TODO("length") ;
}
static ugly_status dn_get_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want, ugly_val *out){
	TODO("get_index") ;
}
static ugly_status dn_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	TODO("set_index") ;
}
static ugly_status dn_call(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out){
	TODO("call") ;
}
static ugly_status dn_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out){
	TODO("invoke") ;
}
static ugly_status dn_new_instance(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_handle *out){
	TODO("new_instance") ;
}
static ugly_status dn_delete_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name){
	TODO("delete_member") ;
}
static ugly_status dn_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n){
	TODO("splice") ;
}


/* No refs are handed out yet. */
static void dn_release(ugly_rt *rt, void *ref){
}


static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = 0,
	.name = "dotnet",
	.init = dn_init,
	.rt_new = dn_rt_new,
	.rt_delete = dn_rt_delete,
	.import = dn_import,
	.globals = dn_globals,
	.describe = dn_describe,
	.get_member = dn_get_member,
	.set_member = dn_set_member,
	.member_names = dn_member_names,
	.length = dn_length,
	.get_index = dn_get_index,
	.set_index = dn_set_index,
	.call = dn_call,
	.invoke = dn_invoke,
	.new_instance = dn_new_instance,
	.delete_member = dn_delete_member,
	.splice = dn_splice,
	.release = dn_release,
} ;


const ugly_plugin *ugly_plugin_dotnet(void){
	return &plugin ;
}
