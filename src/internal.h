/* UGLY v2 core: internal types shared by the core's source files. */
#ifndef UGLY_INTERNAL_H
#define UGLY_INTERNAL_H

#include <pthread.h>
#include <stdarg.h>

#include "ugly.h"
#include "ugly_plugin.h"


/* A call frame: one per call from the core into a plugin, on this thread's ctx. */
typedef struct frame {
	struct frame *up ;
	ugly_handle *borrows ;       /* borrow slots minted for this call's arguments */
	size_t nborrows, cap ;
	ugly_handle inline_borrows[8] ;
} frame ;

struct ugly_ctx {
	pthread_t thread ;
	ugly_error *error ;          /* the last call's error, or NULL */
	frame *top ;                 /* innermost active call */
	int depth ;                  /* active frames */
	ugly_rt *host ;              /* home of this thread's C callables, lists and maps */

	/* Runtimes ugly_rt_get created (deleted with the ctx), and whether it loaded their language. */
	struct auto_rt { ugly_rt *rt ; ugly_lang *loaded ; } *autos ;
	size_t nautos, autos_cap ;
} ;

struct ugly_error {
	ugly_status status ;
	char *language ;
	char *type ;
	char *message ;
	ugly_handle payload ;        /* owned, or UGLY_NO_HANDLE */
} ;

struct ugly_lang {
	struct ugly_lang *next ;     /* the process-wide list of loaded languages */
	char *name ;
	char *dir ;
	void *dl ;                   /* dlopen handle, NULL for built-in languages */
	const ugly_plugin *plugin ;
	int loads ;                  /* outstanding ugly_lang_load calls */
	int runtimes ;               /* live runtimes */
	int initialized ;
	ugly_rt *rts ;               /* live runtimes, linked through ugly_rt.lang_next (langs lock) */
} ;

struct ugly_rt {
	ugly_lang *lang ;
	struct ugly_rt *lang_next ;
	void *native ;
	pthread_t thread ;
	int depth ;                  /* calls into this runtime in progress */
	int dying ;                  /* ugly_rt_delete in progress */

	/* Native refs whose last handle reference was dropped on another thread
	 * (protected by the handle table's lock). */
	void **pending ;
	size_t npending, pending_cap ;
} ;


/* ctx.c */
ugly_status ctx_check(ugly_ctx *ctx) ;                      /* right thread; clears the error */
ugly_status ctx_fail(ugly_ctx *ctx, ugly_status status, const char *language, const char *type,
	const char *fmt, ...) ;
void ctx_set_error(ugly_ctx *ctx, ugly_error *err) ;
ugly_error *error_new(ugly_status status, const char *language, const char *type, const char *message,
	ugly_handle payload) ;

/* handles.c */
ugly_status h_export(ugly_rt *rt, void *ref, uintptr_t identity, ugly_handle *out, int *duplicate) ;
ugly_status h_lookup(ugly_handle h, ugly_rt **home, void **ref) ;
ugly_status h_dup(ugly_handle h, ugly_handle *out) ;
ugly_status h_release(ugly_handle h) ;
ugly_status h_borrow(ugly_handle h, ugly_handle *out) ;
void h_revoke(ugly_handle borrow) ;
ugly_status h_same(ugly_handle a, ugly_handle b, int *out) ;
/* Frees every slot whose home is rt; returns the native refs to release (caller frees the array). */
void h_invalidate(ugly_rt *rt, void ***refs, size_t *nrefs) ;
/* Releases the native refs queued for rt by other threads. Call on rt's thread. */
void h_drain(ugly_rt *rt) ;

/* rt.c */
ugly_status rt_create(ugly_ctx *ctx, ugly_lang *lang, void *existing, int attach, ugly_rt **out) ;
ugly_status rt_destroy(ugly_ctx *ctx, ugly_rt *rt) ;
ugly_status rt_enter(ugly_ctx *ctx, ugly_rt *rt, frame *f) ;
void rt_leave(ugly_ctx *ctx, ugly_rt *rt, frame *f) ;
ugly_status frame_borrow_args(ugly_ctx *ctx, frame *f, const ugly_val *in, size_t n, ugly_val *out) ;
ugly_status after_call(ugly_ctx *ctx, ugly_rt *rt, ugly_status st, const char *op, ugly_kind want, ugly_val *out) ;

/* lang.c */
ugly_lang *lang_builtin_c(void) ;
void lang_add_rt(ugly_lang *lang, ugly_rt *rt) ;
void lang_remove_rt(ugly_lang *lang, ugly_rt *rt) ;
ugly_lang *lang_find(const char *name) ;
ugly_rt *lang_find_rt_on_thread(ugly_lang *lang) ;     /* the oldest live runtime owned by this thread */     /* a loaded language, or NULL; no load is counted */

/* host.c: the built-in "c" language (callables, lists, maps) */
extern const ugly_plugin ugly_c_plugin ;

/* Small helpers */
char *xstrdup(const char *s) ;
int on_thread(pthread_t t) ;

#endif
