/*
 * UGLY v2 public API: approved 2026-11-24. See doc/contract.md.
 *
 * This is what a host program (or a language binding acting as one) uses.
 * Plugins implement ugly_plugin.h.
 *
 * Conventions:
 * - Every call takes the calling thread's ugly_ctx and returns a ugly_status.
 *   It clears the ctx's error first; on anything other than UGLY_OK,
 *   ugly_last_error() describes the failure. The exceptions, which leave the
 *   error alone so that cleanup can't erase it, are ugly_release,
 *   ugly_val_release and ugly_error_payload.
 * - Results come back through out-parameters and are owned by the caller:
 *   release them with ugly_val_release() / ugly_release().
 * - Arguments are borrowed: the callee copies what it keeps, including
 *   handles (ugly_dup()).
 */
#ifndef UGLY_H
#define UGLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UGLY_API_VERSION 2

/*
 * The deepest the core lets calls nest (frames on one thread's ctx) before
 * failing with UGLY_ERR_TOO_DEEP, so runaway re-entry is an error rather
 * than a C stack overflow. A language may have a lower limit of its own (Lua:
 * about 200 nested C calls), which its plugin reports as an error.
 */
#define UGLY_MAX_DEPTH 1000


/* ------------------------------------------------------------------ */
/* Status codes                                                       */
/* ------------------------------------------------------------------ */

typedef enum ugly_status {
	UGLY_OK = 0,
	UGLY_ERR_RAISED,           /* a language raised an exception: see the error's payload */
	UGLY_ERR_TYPE,             /* a value can't be converted to the kind asked for */
	UGLY_ERR_NOT_FOUND,        /* no such member, index, library or language */
	UGLY_ERR_NOT_CAPABLE,      /* the handle lacks the trait the operation needs */
	UGLY_ERR_STALE_HANDLE,     /* released, revoked (borrow ended) or its runtime is gone */
	UGLY_ERR_WRONG_THREAD,     /* runtime or ctx used from a thread that doesn't own it */
	UGLY_ERR_IN_USE,           /* can't delete: still has live dependents */
	UGLY_ERR_TOO_DEEP,         /* calls nested past the core's limit (UGLY_MAX_DEPTH) */
	UGLY_ERR_NOT_SUPPORTED,    /* the language can't do this */
	UGLY_ERR_NOT_IMPLEMENTED,  /* the language could, the plugin doesn't (yet) */
	UGLY_ERR_USAGE,            /* invalid arguments to the API itself */
	UGLY_ERR_NOMEM,
	UGLY_ERR_INTERNAL
} ugly_status;


/* ------------------------------------------------------------------ */
/* Opaque types                                                       */
/* ------------------------------------------------------------------ */

typedef struct ugly_ctx ugly_ctx;           /* per thread: error slot, call frames */
typedef struct ugly_lang ugly_lang;         /* a loaded plugin */
typedef struct ugly_rt ugly_rt;             /* one interpreter / VM instance */
typedef struct ugly_error ugly_error;       /* a structured error */

/*
 * A handle names a non-primitive value (object, function, list, map, module,
 * class...) that lives in some runtime, its "home". It is an index plus a
 * generation into the core's handle table, never a pointer: a stale handle is
 * detected (UGLY_ERR_STALE_HANDLE), not dereferenced.
 */
typedef uint64_t ugly_handle;
#define UGLY_NO_HANDLE ((ugly_handle)0)


/* ------------------------------------------------------------------ */
/* Values                                                             */
/* ------------------------------------------------------------------ */

typedef enum ugly_kind {
	UGLY_ANY = 0,      /* only as a "want": let the home language pick */
	UGLY_NULL,
	UGLY_BOOL,
	UGLY_INT,          /* int64 */
	UGLY_DOUBLE,
	UGLY_STRING,       /* UTF-8, explicit length, may contain NUL */
	UGLY_BYTES,
	UGLY_HANDLE
} ugly_kind;

/*
 * Primitives cross by value; everything else crosses as a handle. Passed by
 * value, no allocation for primitives. String and bytes pointers in arguments
 * are borrowed for the duration of the call; in results they point to memory
 * owned by the value (free it with ugly_val_release()).
 */
typedef struct ugly_val {
	ugly_kind kind;
	union {
		bool b;
		int64_t i;
		double d;
		struct { const char *ptr; size_t len; } str;
		struct { const uint8_t *ptr; size_t len; } bytes;
		ugly_handle h;
	} as;
} ugly_val;

/* Constructors for arguments: nothing is copied or allocated. */
static inline ugly_val ugly_null(void){ ugly_val v = { UGLY_NULL, { .i = 0 } } ; return v ; }
static inline ugly_val ugly_bool(bool b){ ugly_val v = { UGLY_BOOL, { .b = b } } ; return v ; }
static inline ugly_val ugly_int(int64_t i){ ugly_val v = { UGLY_INT, { .i = i } } ; return v ; }
static inline ugly_val ugly_double(double d){ ugly_val v = { UGLY_DOUBLE, { .d = d } } ; return v ; }
static inline ugly_val ugly_str(const char *p, size_t len){ ugly_val v = { UGLY_STRING, { .str = { p, len } } } ; return v ; }
static inline ugly_val ugly_bytes(const uint8_t *p, size_t len){ ugly_val v = { UGLY_BYTES, { .bytes = { p, len } } } ; return v ; }
static inline ugly_val ugly_hval(ugly_handle h){ ugly_val v = { UGLY_HANDLE, { .h = h } } ; return v ; }

/* Releases what a result value owns (string/bytes memory, a handle). Sets it to NULL. */
void ugly_val_release(ugly_ctx *ctx, ugly_val *v) ;


/* ------------------------------------------------------------------ */
/* Contexts and errors                                                */
/* ------------------------------------------------------------------ */

/* A ctx belongs to the thread that created it. One per thread: a second ugly_ctx_new on the same thread is UGLY_ERR_USAGE. */
ugly_status ugly_ctx_new(ugly_ctx **out) ;
void ugly_ctx_delete(ugly_ctx *ctx) ;

/* The error from the last call on ctx, or NULL. Valid until the next call on ctx. */
const ugly_error *ugly_last_error(const ugly_ctx *ctx) ;

ugly_status ugly_error_status(const ugly_error *err) ;
const char *ugly_error_language(const ugly_error *err) ;  /* "lua", "python", "perl5", "core", "c"... */
const char *ugly_error_type(const ugly_error *err) ;      /* in that language's terms, e.g. "ZeroDivisionError" */
const char *ugly_error_message(const ugly_error *err) ;

/*
 * The original exception object, as an owned handle, or UGLY_NO_HANDLE.
 * When it travels back to its home runtime, the home plugin rethrows that
 * object itself, so its identity survives the round trip.
 */
ugly_status ugly_error_payload(ugly_ctx *ctx, const ugly_error *err, ugly_handle *out) ;

/*
 * For a C callable (ugly_callable_new) failing: records an error in ctx with
 * language "c" and returns 'status', so a callback can 'return ugly_raise(...)'.
 * Ownership of 'payload' (or UGLY_NO_HANDLE) moves to the error.
 */
ugly_status ugly_raise(ugly_ctx *ctx, ugly_status status, const char *type, const char *message,
	ugly_handle payload) ;


/* ------------------------------------------------------------------ */
/* Languages and runtimes                                             */
/* ------------------------------------------------------------------ */

/* What a plugin can do. Tests skip by capability instead of failing. */
#define UGLY_CAP_MULTI_RUNTIME  (1u << 0)  /* several runtimes per process */
#define UGLY_CAP_RESTARTABLE    (1u << 1)  /* can unload and load again in one process */
#define UGLY_CAP_ATTACH         (1u << 2)  /* can attach to an interpreter it didn't create */
#define UGLY_CAP_EVAL           (1u << 3)  /* supports ugly_rt_eval */

/*
 * Loads ugly_<name>.so by full path, from the first directory in UGLY_PLUGIN_PATH
 * (colon-separated) that has it, else from the compiled-in plugin directory.
 * Loading a language that is already loaded returns the same ugly_lang.
 */
ugly_status ugly_lang_load(ugly_ctx *ctx, const char *name, ugly_lang **out) ;
const char *ugly_lang_name(const ugly_lang *lang) ;
uint32_t ugly_lang_caps(const ugly_lang *lang) ;
/*
 * Undoes one ugly_lang_load. The last one is refused with UGLY_ERR_IN_USE while
 * runtimes are alive. A plugin without UGLY_CAP_RESTARTABLE stays mapped.
 */
ugly_status ugly_lang_unload(ugly_ctx *ctx, ugly_lang *lang) ;

/* A new interpreter, owned by (and bound to) the calling thread. */
ugly_status ugly_rt_new(ugly_ctx *ctx, ugly_lang *lang, ugly_rt **out) ;

/*
 * Wraps an interpreter that already exists (the host's own), so its values
 * can be exported as handles. This is how a language binding works: a binding
 * is that language's plugin in attach mode. 'native' is plugin-specific
 * (a PerlInterpreter*, a lua_State*, ...). Requires UGLY_CAP_ATTACH.
 */
ugly_status ugly_rt_attach(ugly_ctx *ctx, ugly_lang *lang, void *native, ugly_rt **out) ;

/*
 * Destroys the interpreter (or detaches). Live handles whose home is rt are
 * released first, while the interpreter still runs (so finalizers run), and
 * then become stale: proxies for them in other runtimes fail with
 * UGLY_ERR_STALE_HANDLE from then on. Not refused for live handles (see
 * contract.md, section 7), but UGLY_ERR_IN_USE while a call into rt is in
 * progress.
 */
ugly_status ugly_rt_delete(ugly_ctx *ctx, ugly_rt *rt) ;
ugly_lang *ugly_rt_lang(const ugly_rt *rt) ;

/*
 * "The runtime of language 'name' for this thread": the oldest live runtime
 * of it owned by this thread (whoever created it), or else a new one, which
 * the ctx owns and deletes in ugly_ctx_delete (unloading the language if
 * this call loaded it). What a binding's ugly.module(lang, name) uses.
 */
ugly_status ugly_rt_get(ugly_ctx *ctx, const char *name, ugly_rt **out) ;
/* The oldest live runtime of lang owned by the calling thread, or NULL. */
ugly_rt *ugly_rt_find(ugly_ctx *ctx, ugly_lang *lang) ;

/* Loads a library/module ("calc", "os.path", "./x.js") and returns it as a handle. */
ugly_status ugly_rt_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_handle *out) ;
/* The runtime's global namespace (Lua _G, Python __main__, JS globalThis, Perl main::). */
ugly_status ugly_rt_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_handle *out) ;
/* Optional (UGLY_CAP_EVAL). */
ugly_status ugly_rt_eval(ugly_ctx *ctx, ugly_rt *rt, const char *code, size_t len, ugly_kind want, ugly_val *out) ;


/* ------------------------------------------------------------------ */
/* Handles: ownership                                                 */
/* ------------------------------------------------------------------ */

/* Another owned reference to the same value (for keeping a borrowed handle). */
ugly_status ugly_dup(ugly_ctx *ctx, ugly_handle h, ugly_handle *out) ;
/*
 * Drops an owned reference. Safe from any thread, and ctx may be NULL (a GC
 * finalizer thread may have none): off the home thread, the native release
 * is deferred to the next entry into the home runtime.
 */
void ugly_release(ugly_ctx *ctx, ugly_handle h) ;
/* Whether two handles (owned or borrowed) name the same value. */
ugly_status ugly_same(ugly_ctx *ctx, ugly_handle a, ugly_handle b, bool *out) ;


/* ------------------------------------------------------------------ */
/* Handles: the interop protocol                                      */
/* ------------------------------------------------------------------ */

/* What a handle supports. A value can have several traits (a Python list has ELEMENTS and MEMBERS). */
#define UGLY_TRAIT_MEMBERS       (1u << 0)  /* string-keyed: fields, methods, dict/hash/table entries */
#define UGLY_TRAIT_ELEMENTS      (1u << 1)  /* integer-indexed, with a length */
#define UGLY_TRAIT_CALLABLE      (1u << 2)  /* ugly_call */
#define UGLY_TRAIT_INSTANTIABLE  (1u << 3)  /* ugly_new: classes, constructors */

/* Traits, plus the type name in the home language's terms ("dict", "HASH", "table"). */
ugly_status ugly_describe(ugly_ctx *ctx, ugly_handle h, uint32_t *traits, ugly_val *type_name) ;

ugly_status ugly_get_member(ugly_ctx *ctx, ugly_handle h, const char *name, ugly_kind want, ugly_val *out) ;
ugly_status ugly_set_member(ugly_ctx *ctx, ugly_handle h, const char *name, ugly_val v) ;
/*
 * Removes a member (Python del, Lua t[k] = nil, JS delete, Perl delete,
 * Map.remove). A member that isn't there is not an error.
 * UGLY_ERR_NOT_CAPABLE if the value's members can't be removed.
 */
ugly_status ugly_delete_member(ugly_ctx *ctx, ugly_handle h, const char *name) ;
/* The member names, as a core list handle of strings. */
ugly_status ugly_member_names(ugly_ctx *ctx, ugly_handle h, ugly_handle *out) ;

ugly_status ugly_length(ugly_ctx *ctx, ugly_handle h, int64_t *out) ;
ugly_status ugly_get_index(ugly_ctx *ctx, ugly_handle h, int64_t i, ugly_kind want, ugly_val *out) ;
ugly_status ugly_set_index(ugly_ctx *ctx, ugly_handle h, int64_t i, ugly_val v) ;
/*
 * Replaces 'remove' elements from 'offset' with the 'n' values of 'insert',
 * resizing the sequence: append is (length, 0, v, 1), pop is (length - 1,
 * 1, NULL, 0), clear is (0, length, NULL, 0). The range must lie within
 * 0..length, otherwise UGLY_ERR_NOT_FOUND (the core checks it, with
 * ugly_length, before the plugin runs). UGLY_ERR_NOT_CAPABLE for a sequence
 * of fixed size (a Java array, a Python tuple).
 */
ugly_status ugly_splice(ugly_ctx *ctx, ugly_handle h, int64_t offset, int64_t remove, const ugly_val *insert, size_t n) ;

/*
 * 'want' is the kind the caller needs back. UGLY_ANY lets the home language
 * pick its natural kind; anything else is converted by the home plugin with
 * that language's own rules (a Perl "42" becomes INT; a Python "42" is
 * UGLY_ERR_TYPE).
 */
ugly_status ugly_call(ugly_ctx *ctx, ugly_handle fn, const ugly_val *args, size_t nargs, ugly_kind want, ugly_val *out) ;
/* A method call, with the language's own receiver semantics (self, this, invocant, ':'). */
ugly_status ugly_invoke(ugly_ctx *ctx, ugly_handle obj, const char *method, const ugly_val *args, size_t nargs, ugly_kind want, ugly_val *out) ;
/* Constructs an instance (Python Class(), JS new, Perl Class->new, Lua Class.new). */
ugly_status ugly_new(ugly_ctx *ctx, ugly_handle cls, const ugly_val *args, size_t nargs, ugly_handle *out) ;


/* ------------------------------------------------------------------ */
/* Values made by the host                                            */
/* ------------------------------------------------------------------ */

/*
 * A C function as a callable handle: the callback mechanism. Any runtime can
 * call it like a native function. Its home is the calling thread's host
 * runtime. free_data runs when the last reference is released.
 */
typedef ugly_status (*ugly_c_fn)(ugly_ctx *ctx, void *data, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out) ;
ugly_status ugly_callable_new(ugly_ctx *ctx, ugly_c_fn fn, void *data, void (*free_data)(void *),
	ugly_handle *out) ;

/* Core-owned containers that implement the protocol, so a host can pass a list or a map to any runtime. */
ugly_status ugly_list_new(ugly_ctx *ctx, const ugly_val *items, size_t n, ugly_handle *out) ;
ugly_status ugly_map_new(ugly_ctx *ctx, ugly_handle *out) ;

/* For a C function returning a value it computed: copies into memory the result owns. */
ugly_status ugly_val_set_string(ugly_ctx *ctx, ugly_val *out, const char *p, size_t len) ;
ugly_status ugly_val_set_bytes(ugly_ctx *ctx, ugly_val *out, const uint8_t *p, size_t len) ;


#ifdef __cplusplus
}
#endif

#endif
