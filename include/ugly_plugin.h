/*
 * UGLY v2 plugin contract: approved 2026-11-24. See doc/v2/contract.md.
 *
 * A plugin is ugly_<lang>.so exporting one symbol:
 *
 *     const ugly_plugin *ugly_plugin_<lang>(void) ;
 *
 * The core speaks to the plugin in native references ('void *ref', whatever
 * keeps the value alive in that runtime: an SV*, a PyObject*, a Lua registry
 * slot, a JSValue box). Handles exist only on the core's side of this
 * interface: the core turns refs into handles (ugly_export) and resolves
 * handles back to refs (ugly_resolve).
 *
 * Unlike v1, this vtable is written by hand, not generated from the public
 * API: the two no longer map 1:1 (the core owns the handle table, borrow
 * revocation, error routing and thread checks).
 */
#ifndef UGLY_PLUGIN_H
#define UGLY_PLUGIN_H

#include "ugly.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UGLY_PLUGIN_ABI 1


/*
 * Rules every plugin follows (contract.md has the reasoning):
 *
 *  1. Re-entry: every entry point makes its runtime current on entry and
 *     restores the previous state on exit (perl context, GIL / thread state,
 *     JNI attachment). Calls may nest across any runtimes, up to
 *     UGLY_MAX_DEPTH (the core's limit) or the language's own limit, which
 *     the plugin reports as an error, never as a crash.
 *  2. Arguments are borrowed. A handle argument that the plugin keeps (stored
 *     in a proxy, captured by a closure) must be ugly_dup()ed first.
 *  3. Results are owned by the caller. String/bytes results are copied with
 *     ugly_val_set_string()/ugly_val_set_bytes().
 *  4. Proxies: a foreign handle appears natively as a proxy object that owns
 *     one handle reference and releases it from its finalizer (__gc,
 *     tp_dealloc, DESTROY, JS finalizer).
 *  5. Unwrapping: when converting a native value that is one of the plugin's
 *     own proxies, emit the proxied handle, never a proxy of a proxy. When
 *     a handle whose home is this runtime arrives, use the original native
 *     value (ugly_resolve), never a proxy.
 *  6. Identity: export the same native object with the same identity key, so
 *     the core returns the same handle while it is live.
 *  7. Errors: a native exception escaping to the core becomes a ugly_error
 *     (ugly_raise_native) whose payload is the exception object. A ugly_error
 *     raised into the language is thrown natively as a wrapper (ugly_take_error)
 *     that the plugin recognises on the way out and returns unchanged. If the
 *     payload's home is this runtime, the original exception object is
 *     thrown instead.
 *  8. Threads: the core has already checked that the caller is the runtime's
 *     thread. 'release' is the one exception (see below).
 */


typedef struct ugly_plugin {
	uint32_t abi ;           /* UGLY_PLUGIN_ABI */
	uint32_t caps ;          /* UGLY_CAP_* */
	const char *name ;       /* "lua", "python", "quickjs", "perl5" */

	/*
	 * Process-wide setup, once per process (or again after 'term'). 'dir' is
	 * the directory the plugin's .so was loaded from; helper files live there. 'term' is called on unload only if UGLY_CAP_RESTARTABLE; a
	 * plugin whose process-wide state is one-way (perl, CPython) leaves it
	 * NULL and stays mapped.
	 */
	ugly_status (*init)(ugly_ctx *ctx, const char *dir) ;
	void (*term)(void) ;

	/* Runtimes. 'native' is the plugin's own per-runtime state. */
	ugly_status (*rt_new)(ugly_ctx *ctx, ugly_rt *rt, void **native) ;
	ugly_status (*rt_attach)(ugly_ctx *ctx, ugly_rt *rt, void *existing, void **native) ;   /* UGLY_CAP_ATTACH */
	ugly_status (*rt_delete)(ugly_ctx *ctx, ugly_rt *rt, void *native) ;

	ugly_status (*import)(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out) ;
	ugly_status (*globals)(ugly_ctx *ctx, ugly_rt *rt, ugly_val *out) ;
	ugly_status (*eval)(ugly_ctx *ctx, ugly_rt *rt, const char *code, size_t len, ugly_kind want,
		ugly_val *out) ;   /* UGLY_CAP_EVAL */

	/* The interop protocol, on values whose home is this runtime. */
	ugly_status (*describe)(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name) ;
	ugly_status (*get_member)(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want, ugly_val *out) ;
	ugly_status (*set_member)(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v) ;
	ugly_status (*member_names)(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out) ;
	ugly_status (*length)(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out) ;
	ugly_status (*get_index)(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want, ugly_val *out) ;
	ugly_status (*set_index)(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v) ;
	ugly_status (*call)(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
		ugly_kind want, ugly_val *out) ;
	ugly_status (*invoke)(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
		size_t nargs, ugly_kind want, ugly_val *out) ;
	ugly_status (*new_instance)(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
		ugly_handle *out) ;

	/*
	 * Drops the plugin's reference. Always called on the runtime's thread:
	 * releases from other threads are queued by the core and drained at the
	 * next entry into this runtime. May be called while the runtime is in
	 * the middle of a call (a finalizer releasing a proxy): it must be safe
	 * to re-enter, or defer the work itself.
	 */
	void (*release)(ugly_rt *rt, void *ref) ;
} ugly_plugin ;


/* ------------------------------------------------------------------ */
/* Core services for plugins                                          */
/* ------------------------------------------------------------------ */

/* The calling thread's ctx, for native code (a proxy's __call) that wasn't handed one. NULL if none. */
ugly_ctx *ugly_current_ctx(void) ;

/* The plugin's per-runtime state, as returned by rt_new/rt_attach. */
void *ugly_rt_native(const ugly_rt *rt) ;

/*
 * Registers a native reference as a new owned handle whose home is rt.
 * 'identity' (non-zero) is the native object's identity (e.g. the PyObject*
 * or the referent SV*): if a live handle in rt already has it, that handle
 * is returned with one more reference, and the core calls release(ref) on
 * the duplicate ref. Zero means "no identity" (always a new handle).
 */
ugly_status ugly_export(ugly_ctx *ctx, ugly_rt *rt, void *ref, uintptr_t identity, ugly_handle *out) ;

/* For a handle whose home is rt: the plugin's native ref. Otherwise *ref = NULL (it's foreign: make a proxy). */
ugly_status ugly_resolve(ugly_ctx *ctx, ugly_rt *rt, ugly_handle h, void **ref) ;

/*
 * A native exception escaping into the core. Records the error in ctx and
 * returns UGLY_ERR_RAISED. 'payload_ref' (the exception object, or NULL) is
 * exported with ugly_export and owned by the error.
 */
ugly_status ugly_raise_native(ugly_ctx *ctx, ugly_rt *rt, const char *type, const char *message,
	void *payload_ref, uintptr_t payload_identity) ;

/*
 * A failure that isn't a language exception (no such member, a value of the
 * wrong type...): records an error in this runtime's language with the given
 * status and no payload, and returns the status.
 */
ugly_status ugly_fail(ugly_ctx *ctx, ugly_rt *rt, ugly_status status, const char *type, const char *message) ;

/*
 * Like ugly_raise_native, for a payload the plugin holds as a handle (a
 * foreign exception rethrown by native code, rule 5). Ownership of 'payload'
 * moves to the error. Returns UGLY_ERR_RAISED.
 */
ugly_status ugly_raise_handle(ugly_ctx *ctx, ugly_rt *rt, const char *type, const char *message, ugly_handle payload) ;

/* Takes the current error out of ctx, for wrapping in a native exception. Caller owns it. */
ugly_error *ugly_take_error(ugly_ctx *ctx) ;
/* Puts a previously taken error back into ctx (it left the language unhandled). Returns its status. */
ugly_status ugly_restore_error(ugly_ctx *ctx, ugly_error *err) ;
/* Frees a taken error that the language handled (caught) and dropped. */
void ugly_error_free(ugly_error *err) ;


#ifdef __cplusplus
}
#endif

#endif
