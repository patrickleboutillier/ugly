/* Runtimes, and the frame discipline around every call into a plugin. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"


static const char *kind_name(ugly_kind k){
	switch (k){
		case UGLY_ANY: return "ANY" ;
		case UGLY_NULL: return "NULL" ;
		case UGLY_BOOL: return "BOOL" ;
		case UGLY_INT: return "INT" ;
		case UGLY_DOUBLE: return "DOUBLE" ;
		case UGLY_STRING: return "STRING" ;
		case UGLY_BYTES: return "BYTES" ;
		case UGLY_HANDLE: return "HANDLE" ;
	}
	return "?" ;
}


/* ---------------------------------------------------------------- */
/* Frames                                                           */
/* ---------------------------------------------------------------- */

ugly_status rt_enter(ugly_ctx *ctx, ugly_rt *rt, frame *f){
	if (! on_thread(rt->thread)){
		return ctx_fail(ctx, UGLY_ERR_WRONG_THREAD, "core", "", "runtime of %s used from another thread",
			rt->lang->name) ;
	}
	if (rt->dying){
		return ctx_fail(ctx, UGLY_ERR_STALE_HANDLE, "core", "", "runtime of %s is being deleted",
			rt->lang->name) ;
	}
	memset(f, 0, sizeof(frame)) ;
	f->borrows = f->inline_borrows ;
	f->cap = sizeof(f->inline_borrows) / sizeof(f->inline_borrows[0]) ;
	f->up = ctx->top ;
	ctx->top = f ;
	h_drain(rt) ;
	rt->depth++ ;
	return UGLY_OK ;
}


void rt_leave(ugly_ctx *ctx, ugly_rt *rt, frame *f){
	rt->depth-- ;
	size_t i ;
	for (i = 0 ; i < f->nborrows ; i++){
		h_revoke(f->borrows[i]) ;
	}
	if (f->borrows != f->inline_borrows){
		free(f->borrows) ;
	}
	ctx->top = f->up ;
}


/* Copies args into out, replacing each handle with a borrow that ends with this frame. */
ugly_status frame_borrow_args(ugly_ctx *ctx, frame *f, const ugly_val *in, size_t n, ugly_val *out){
	size_t i ;
	for (i = 0 ; i < n ; i++){
		out[i] = in[i] ;
		if (in[i].kind != UGLY_HANDLE){
			continue ;
		}
		if (f->nborrows == f->cap){
			size_t ncap = f->cap * 2 ;
			ugly_handle *nb = malloc(ncap * sizeof(ugly_handle)) ;
			if (nb == NULL){
				return ctx_fail(ctx, UGLY_ERR_NOMEM, "core", "", "out of memory") ;
			}
			memcpy(nb, f->borrows, f->nborrows * sizeof(ugly_handle)) ;
			if (f->borrows != f->inline_borrows){
				free(f->borrows) ;
			}
			f->borrows = nb ;
			f->cap = ncap ;
		}
		ugly_handle b ;
		ugly_status st = h_borrow(in[i].as.h, &b) ;
		if (st != UGLY_OK){
			return ctx_fail(ctx, st, "core", "", "argument %zu: %s", i + 1,
				st == UGLY_ERR_STALE_HANDLE ? "stale handle" : "out of memory") ;
		}
		f->borrows[f->nborrows++] = b ;
		out[i].as.h = b ;
	}
	return UGLY_OK ;
}


/*
 * Normalizes a plugin's result: a failure always carries an error, a success
 * never does, and a success honours 'want' (the plugin's job; the core only
 * checks it).
 */
ugly_status after_call(ugly_ctx *ctx, ugly_rt *rt, ugly_status st, const char *op, ugly_kind want, ugly_val *out){
	if (st != UGLY_OK){
		if (out != NULL){
			ugly_val_release(ctx, out) ;
		}
		if (ctx->error == NULL){
			ctx_fail(ctx, st, rt->lang->name, "", "%s failed", op) ;
		}
		return st ;
	}
	if (ctx->error != NULL){
		/* An inner error the plugin handled but didn't take. */
		ugly_error_free(ctx->error) ;
		ctx->error = NULL ;
	}
	if ((out != NULL)&&(want != UGLY_ANY)&&(out->kind != want)){
		ugly_kind got = out->kind ;
		ugly_val_release(ctx, out) ;
		return ctx_fail(ctx, UGLY_ERR_INTERNAL, "core", "", "plugin %s: %s returned %s when %s was wanted",
			rt->lang->name, op, kind_name(got), kind_name(want)) ;
	}
	return UGLY_OK ;
}


/* ---------------------------------------------------------------- */
/* Creating and destroying                                          */
/* ---------------------------------------------------------------- */

ugly_status rt_create(ugly_ctx *ctx, ugly_lang *lang, void *existing, int attach, ugly_rt **out){
	ugly_rt *rt = calloc(1, sizeof(ugly_rt)) ;
	if (rt == NULL){
		return ctx_fail(ctx, UGLY_ERR_NOMEM, "core", "", "out of memory") ;
	}
	rt->lang = lang ;
	rt->thread = pthread_self() ;

	const ugly_plugin *p = lang->plugin ;
	ugly_status st = (attach ? p->rt_attach(ctx, rt, existing, &rt->native) : p->rt_new(ctx, rt, &rt->native)) ;
	if (st != UGLY_OK){
		if (ctx->error == NULL){
			ctx_fail(ctx, st, lang->name, "", "can't create a runtime") ;
		}
		free(rt) ;
		return st ;
	}
	__atomic_add_fetch(&lang->runtimes, 1, __ATOMIC_SEQ_CST) ;
	*out = rt ;
	return UGLY_OK ;
}


ugly_status rt_destroy(ugly_ctx *ctx, ugly_rt *rt){
	if (rt->depth > 0){
		return ctx_fail(ctx, UGLY_ERR_IN_USE, "core", "", "can't delete a runtime of %s during a call into it",
			rt->lang->name) ;
	}

	/*
	 * Release every live value while the interpreter still runs, so
	 * finalizers see a working runtime. Finalizers can create values too, so
	 * repeat until nothing is left; only then mark the runtime as dying.
	 */
	h_drain(rt) ;
	int round ;
	for (round = 0 ; round < 100 ; round++){
		void **refs ;
		size_t n, i ;
		h_invalidate(rt, &refs, &n) ;
		for (i = 0 ; i < n ; i++){
			rt->lang->plugin->release(rt, refs[i]) ;
		}
		free(refs) ;
		h_drain(rt) ;
		if (n == 0){
			break ;
		}
	}
	rt->dying = 1 ;

	ugly_status st = rt->lang->plugin->rt_delete(ctx, rt, rt->native) ;
	__atomic_sub_fetch(&rt->lang->runtimes, 1, __ATOMIC_SEQ_CST) ;
	free(rt->pending) ;
	free(rt) ;
	return st ;
}


/* ---------------------------------------------------------------- */
/* Public API                                                       */
/* ---------------------------------------------------------------- */

ugly_status ugly_rt_new(ugly_ctx *ctx, ugly_lang *lang, ugly_rt **out){
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if ((lang == NULL)||(out == NULL)||(lang->loads <= 0)){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_rt_new: invalid language or out") ;
	}
	*out = NULL ;
	if ((! (lang->plugin->caps & UGLY_CAP_MULTI_RUNTIME))&&(lang->runtimes > 0)){
		return ctx_fail(ctx, UGLY_ERR_NOT_SUPPORTED, lang->name, "", "%s supports one runtime per process",
			lang->name) ;
	}
	return rt_create(ctx, lang, NULL, 0, out) ;
}


ugly_status ugly_rt_attach(ugly_ctx *ctx, ugly_lang *lang, void *native, ugly_rt **out){
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if ((lang == NULL)||(out == NULL)||(lang->loads <= 0)){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_rt_attach: invalid language or out") ;
	}
	*out = NULL ;
	if ((! (lang->plugin->caps & UGLY_CAP_ATTACH))||(lang->plugin->rt_attach == NULL)){
		return ctx_fail(ctx, UGLY_ERR_NOT_SUPPORTED, lang->name, "", "%s can't attach to an existing interpreter",
			lang->name) ;
	}
	return rt_create(ctx, lang, native, 1, out) ;
}


ugly_status ugly_rt_delete(ugly_ctx *ctx, ugly_rt *rt){
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if (rt == NULL){
		return UGLY_OK ;
	}
	if (rt == ctx->host){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "the host runtime is deleted with its ctx") ;
	}
	if (! on_thread(rt->thread)){
		return ctx_fail(ctx, UGLY_ERR_WRONG_THREAD, "core", "", "runtime deleted from another thread") ;
	}
	return rt_destroy(ctx, rt) ;
}


ugly_lang *ugly_rt_lang(const ugly_rt *rt){
	return rt->lang ;
}


/* import/globals/eval: same frame discipline as the protocol operations. */
static ugly_status rt_op(ugly_ctx *ctx, ugly_rt *rt, const char *op, const char *name, const char *code,
	size_t len, ugly_kind want, ugly_val *out){

	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if ((rt == NULL)||(out == NULL)){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "%s: invalid arguments", op) ;
	}
	*out = ugly_null() ;
	const ugly_plugin *p = rt->lang->plugin ;
	frame f ;
	st = rt_enter(ctx, rt, &f) ;
	if (st != UGLY_OK){
		return st ;
	}
	if (strcmp(op, "import") == 0){
		st = (p->import ? p->import(ctx, rt, name, out) : UGLY_ERR_NOT_IMPLEMENTED) ;
	}
	else if (strcmp(op, "globals") == 0){
		st = (p->globals ? p->globals(ctx, rt, out) : UGLY_ERR_NOT_IMPLEMENTED) ;
	}
	else if ((p->eval != NULL)&&(p->caps & UGLY_CAP_EVAL)){
		st = p->eval(ctx, rt, code, len, want, out) ;
	}
	else {
		st = UGLY_ERR_NOT_SUPPORTED ;
	}
	rt_leave(ctx, rt, &f) ;
	return after_call(ctx, rt, st, op, want, out) ;
}


ugly_status ugly_rt_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_handle *out){
	if (out != NULL){
		*out = UGLY_NO_HANDLE ;
	}
	ugly_val v ;
	ugly_status st = rt_op(ctx, rt, "import", name, NULL, 0, UGLY_HANDLE, (out != NULL ? &v : NULL)) ;
	if (st == UGLY_OK){
		*out = v.as.h ;
	}
	return st ;
}


ugly_status ugly_rt_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_handle *out){
	if (out != NULL){
		*out = UGLY_NO_HANDLE ;
	}
	ugly_val v ;
	ugly_status st = rt_op(ctx, rt, "globals", NULL, NULL, 0, UGLY_HANDLE, (out != NULL ? &v : NULL)) ;
	if (st == UGLY_OK){
		*out = v.as.h ;
	}
	return st ;
}


ugly_status ugly_rt_eval(ugly_ctx *ctx, ugly_rt *rt, const char *code, size_t len, ugly_kind want, ugly_val *out){
	return rt_op(ctx, rt, "eval", NULL, code, len, want, out) ;
}


void *ugly_rt_native(const ugly_rt *rt){
	return rt->native ;
}
