/*
 * The handle protocol: every operation resolves its target to (home runtime,
 * native ref), enters the home runtime (thread check, frame, deferred
 * releases), borrows its handle arguments, calls the home plugin, and
 * normalizes the result.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"


typedef struct call {
	ugly_ctx *ctx ;
	ugly_rt *rt ;
	void *ref ;
	const ugly_plugin *p ;
	frame f ;
	ugly_val *bargs ;            /* args with handles replaced by borrows */
	ugly_val inline_args[8] ;
} call ;


static ugly_status begin(call *c, ugly_ctx *ctx, ugly_handle h, const ugly_val *args, size_t nargs, ugly_val *out){
	c->ctx = ctx ;
	c->rt = NULL ;
	c->bargs = NULL ;
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if (out != NULL){
		*out = ugly_null() ;
	}
	if ((nargs > 0)&&(args == NULL)){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "args is NULL") ;
	}
	if (h_lookup(h, &c->rt, &c->ref) != UGLY_OK){
		return ctx_fail(ctx, UGLY_ERR_STALE_HANDLE, "core", "", "stale or invalid handle") ;
	}
	c->p = c->rt->lang->plugin ;
	st = rt_enter(ctx, c->rt, &c->f) ;
	if (st != UGLY_OK){
		c->rt = NULL ;
		return st ;
	}
	c->bargs = (nargs <= 8 ? c->inline_args : malloc(nargs * sizeof(ugly_val))) ;
	if (c->bargs == NULL){
		return ctx_fail(ctx, UGLY_ERR_NOMEM, "core", "", "out of memory") ;
	}
	return frame_borrow_args(ctx, &c->f, args, nargs, c->bargs) ;
}


/* Leaves the runtime (revoking borrows) and normalizes the result. */
static ugly_status end(call *c, ugly_status st, const char *op, ugly_kind want, ugly_val *out){
	if (c->bargs != c->inline_args){
		free(c->bargs) ;
	}
	if (c->rt == NULL){
		return st ;          /* never entered */
	}
	rt_leave(c->ctx, c->rt, &c->f) ;
	return after_call(c->ctx, c->rt, st, op, want, out) ;
}


#define NOT_IMPL(fn) ((fn) == NULL ? UGLY_ERR_NOT_IMPLEMENTED : UGLY_OK)


ugly_status ugly_describe(ugly_ctx *ctx, ugly_handle h, uint32_t *traits, ugly_val *type_name){
	call c ;
	ugly_status st = begin(&c, ctx, h, NULL, 0, type_name) ;
	if (st == UGLY_OK){
		if ((traits == NULL)||(type_name == NULL)){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_describe: traits and type_name are required") ;
		}
		else if ((st = NOT_IMPL(c.p->describe)) == UGLY_OK){
			*traits = 0 ;
			st = c.p->describe(ctx, c.rt, c.ref, traits, type_name) ;
		}
	}
	return end(&c, st, "describe", UGLY_STRING, type_name) ;
}


ugly_status ugly_get_member(ugly_ctx *ctx, ugly_handle h, const char *name, ugly_kind want, ugly_val *out){
	call c ;
	ugly_status st = begin(&c, ctx, h, NULL, 0, out) ;
	if (st == UGLY_OK){
		if ((name == NULL)||(out == NULL)){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_get_member: name and out are required") ;
		}
		else if ((st = NOT_IMPL(c.p->get_member)) == UGLY_OK){
			st = c.p->get_member(ctx, c.rt, c.ref, name, want, out) ;
		}
	}
	return end(&c, st, "get_member", want, out) ;
}


ugly_status ugly_set_member(ugly_ctx *ctx, ugly_handle h, const char *name, ugly_val v){
	call c ;
	ugly_status st = begin(&c, ctx, h, &v, 1, NULL) ;
	if (st == UGLY_OK){
		if (name == NULL){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_set_member: name is required") ;
		}
		else if ((st = NOT_IMPL(c.p->set_member)) == UGLY_OK){
			st = c.p->set_member(ctx, c.rt, c.ref, name, c.bargs[0]) ;
		}
	}
	return end(&c, st, "set_member", UGLY_ANY, NULL) ;
}


ugly_status ugly_member_names(ugly_ctx *ctx, ugly_handle h, ugly_handle *out){
	call c ;
	ugly_val v = ugly_null() ;
	if (out != NULL){
		*out = UGLY_NO_HANDLE ;
	}
	ugly_status st = begin(&c, ctx, h, NULL, 0, NULL) ;
	if (st == UGLY_OK){
		if (out == NULL){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_member_names: out is required") ;
		}
		else if ((st = NOT_IMPL(c.p->member_names)) == UGLY_OK){
			ugly_handle names = UGLY_NO_HANDLE ;
			st = c.p->member_names(ctx, c.rt, c.ref, &names) ;
			if (st == UGLY_OK){
				v = ugly_hval(names) ;
			}
		}
	}
	st = end(&c, st, "member_names", UGLY_HANDLE, &v) ;
	if (st == UGLY_OK){
		*out = v.as.h ;
	}
	return st ;
}


ugly_status ugly_length(ugly_ctx *ctx, ugly_handle h, int64_t *out){
	call c ;
	ugly_status st = begin(&c, ctx, h, NULL, 0, NULL) ;
	if (st == UGLY_OK){
		if (out == NULL){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_length: out is required") ;
		}
		else if ((st = NOT_IMPL(c.p->length)) == UGLY_OK){
			st = c.p->length(ctx, c.rt, c.ref, out) ;
		}
	}
	return end(&c, st, "length", UGLY_ANY, NULL) ;
}


ugly_status ugly_get_index(ugly_ctx *ctx, ugly_handle h, int64_t i, ugly_kind want, ugly_val *out){
	call c ;
	ugly_status st = begin(&c, ctx, h, NULL, 0, out) ;
	if (st == UGLY_OK){
		if (out == NULL){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_get_index: out is required") ;
		}
		else if ((st = NOT_IMPL(c.p->get_index)) == UGLY_OK){
			st = c.p->get_index(ctx, c.rt, c.ref, i, want, out) ;
		}
	}
	return end(&c, st, "get_index", want, out) ;
}


ugly_status ugly_set_index(ugly_ctx *ctx, ugly_handle h, int64_t i, ugly_val v){
	call c ;
	ugly_status st = begin(&c, ctx, h, &v, 1, NULL) ;
	if ((st == UGLY_OK)&&((st = NOT_IMPL(c.p->set_index)) == UGLY_OK)){
		st = c.p->set_index(ctx, c.rt, c.ref, i, c.bargs[0]) ;
	}
	return end(&c, st, "set_index", UGLY_ANY, NULL) ;
}


ugly_status ugly_delete_member(ugly_ctx *ctx, ugly_handle h, const char *name){
	call c ;
	ugly_status st = begin(&c, ctx, h, NULL, 0, NULL) ;
	if (st == UGLY_OK){
		if (name == NULL){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_delete_member: name is required") ;
		}
		else if ((st = NOT_IMPL(c.p->delete_member)) == UGLY_OK){
			st = c.p->delete_member(ctx, c.rt, c.ref, name) ;
		}
	}
	return end(&c, st, "delete_member", UGLY_ANY, NULL) ;
}


ugly_status ugly_splice(ugly_ctx *ctx, ugly_handle h, int64_t offset, int64_t remove, const ugly_val *insert, size_t n){
	call c ;
	ugly_status st = begin(&c, ctx, h, insert, n, NULL) ;
	if (st == UGLY_OK){
		int64_t len = 0 ;
		if (((st = NOT_IMPL(c.p->splice)) == UGLY_OK)&&((st = NOT_IMPL(c.p->length)) == UGLY_OK)
			&&((st = c.p->length(ctx, c.rt, c.ref, &len)) == UGLY_OK)){
			if ((offset < 0)||(remove < 0)||(offset > len)||(remove > len - offset)){
				st = ctx_fail(ctx, UGLY_ERR_NOT_FOUND, "core", "", "ugly_splice: %lld..%lld is outside 0..%lld",
					(long long)offset, (long long)offset + (long long)remove, (long long)len) ;
			}
			else {
				st = c.p->splice(ctx, c.rt, c.ref, offset, remove, c.bargs, n) ;
			}
		}
	}
	return end(&c, st, "splice", UGLY_ANY, NULL) ;
}


ugly_status ugly_call(ugly_ctx *ctx, ugly_handle fn, const ugly_val *args, size_t nargs, ugly_kind want, ugly_val *out){
	call c ;
	ugly_status st = begin(&c, ctx, fn, args, nargs, out) ;
	if (st == UGLY_OK){
		if (out == NULL){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_call: out is required") ;
		}
		else if ((st = NOT_IMPL(c.p->call)) == UGLY_OK){
			st = c.p->call(ctx, c.rt, c.ref, c.bargs, nargs, want, out) ;
		}
	}
	return end(&c, st, "call", want, out) ;
}


ugly_status ugly_invoke(ugly_ctx *ctx, ugly_handle obj, const char *method, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out){

	call c ;
	ugly_status st = begin(&c, ctx, obj, args, nargs, out) ;
	if (st == UGLY_OK){
		if ((method == NULL)||(out == NULL)){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_invoke: method and out are required") ;
		}
		else if ((st = NOT_IMPL(c.p->invoke)) == UGLY_OK){
			st = c.p->invoke(ctx, c.rt, c.ref, method, c.bargs, nargs, want, out) ;
		}
	}
	return end(&c, st, "invoke", want, out) ;
}


ugly_status ugly_new(ugly_ctx *ctx, ugly_handle cls, const ugly_val *args, size_t nargs, ugly_handle *out){
	call c ;
	ugly_val v = ugly_null() ;
	if (out != NULL){
		*out = UGLY_NO_HANDLE ;
	}
	ugly_status st = begin(&c, ctx, cls, args, nargs, NULL) ;
	if (st == UGLY_OK){
		if (out == NULL){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_new: out is required") ;
		}
		else if ((st = NOT_IMPL(c.p->new_instance)) == UGLY_OK){
			ugly_handle h = UGLY_NO_HANDLE ;
			st = c.p->new_instance(ctx, c.rt, c.ref, c.bargs, nargs, &h) ;
			if (st == UGLY_OK){
				v = ugly_hval(h) ;
			}
		}
	}
	st = end(&c, st, "new", UGLY_HANDLE, &v) ;
	if (st == UGLY_OK){
		*out = v.as.h ;
	}
	return st ;
}


/* ---------------------------------------------------------------- */
/* Call options (contract §11)                                      */
/* ---------------------------------------------------------------- */

/* Any option set? A caller's struct may be older (smaller) than ours. */
static int has_opts(const ugly_opts *o){
	if ((o == NULL)||(o->size < offsetof(ugly_opts, hint))){
		return 0 ;
	}
	return (o->nkwargs > 0)
		||((o->size >= offsetof(ugly_opts, context))&&(o->hint != NULL))
		||((o->size >= sizeof(ugly_opts))&&(o->context != NULL)) ;
}


/*
 * A full-size copy of the caller's options for the plugin, with the keyword
 * values borrowed for this call like its positional arguments. *kw is
 * malloc'd: free it after the call.
 */
static ugly_status borrow_opts(call *c, const ugly_opts *in, ugly_opts *out, ugly_kwarg **kw){
	ugly_opts o = UGLY_OPTS_INIT ;
	o.kwargs = in->kwargs ;
	o.nkwargs = in->nkwargs ;
	if (in->size >= offsetof(ugly_opts, context)){
		o.hint = in->hint ;
	}
	if (in->size >= sizeof(ugly_opts)){
		o.context = in->context ;
	}
	*kw = NULL ;
	if (o.nkwargs > 0){
		if (o.kwargs == NULL){
			return ctx_fail(c->ctx, UGLY_ERR_USAGE, "core", "", "call options: kwargs is NULL") ;
		}
		*kw = calloc(o.nkwargs, sizeof(ugly_kwarg)) ;
		if (*kw == NULL){
			return ctx_fail(c->ctx, UGLY_ERR_NOMEM, "core", "", "out of memory") ;
		}
		size_t i ;
		for (i = 0 ; i < o.nkwargs ; i++){
			if (o.kwargs[i].name == NULL){
				return ctx_fail(c->ctx, UGLY_ERR_USAGE, "core", "", "call options: keyword %zu has no name", i) ;
			}
			(*kw)[i].name = o.kwargs[i].name ;
			ugly_status st = frame_borrow_args(c->ctx, &c->f, &o.kwargs[i].value, 1, &(*kw)[i].value) ;
			if (st != UGLY_OK){
				return st ;
			}
		}
		o.kwargs = *kw ;
	}
	*out = o ;
	return UGLY_OK ;
}


static ugly_status no_opts(call *c){
	return ctx_fail(c->ctx, UGLY_ERR_NOT_SUPPORTED, c->rt->lang->name, "",
		"%s takes no call options (keyword arguments, hint, context)", c->rt->lang->name) ;
}


ugly_status ugly_call_opts(ugly_ctx *ctx, ugly_handle fn, const ugly_val *args, size_t nargs,
	const ugly_opts *opts, ugly_kind want, ugly_val *out){

	if (! has_opts(opts)){
		return ugly_call(ctx, fn, args, nargs, want, out) ;
	}
	call c ;
	ugly_kwarg *kw = NULL ;
	ugly_opts o ;
	ugly_status st = begin(&c, ctx, fn, args, nargs, out) ;
	if (st == UGLY_OK){
		if (out == NULL){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_call_opts: out is required") ;
		}
		else if (c.p->call_opts == NULL){
			st = no_opts(&c) ;
		}
		else if ((st = borrow_opts(&c, opts, &o, &kw)) == UGLY_OK){
			st = c.p->call_opts(ctx, c.rt, c.ref, c.bargs, nargs, &o, want, out) ;
		}
	}
	free(kw) ;
	return end(&c, st, "call", want, out) ;
}


ugly_status ugly_invoke_opts(ugly_ctx *ctx, ugly_handle obj, const char *method, const ugly_val *args,
	size_t nargs, const ugly_opts *opts, ugly_kind want, ugly_val *out){

	if (! has_opts(opts)){
		return ugly_invoke(ctx, obj, method, args, nargs, want, out) ;
	}
	call c ;
	ugly_kwarg *kw = NULL ;
	ugly_opts o ;
	ugly_status st = begin(&c, ctx, obj, args, nargs, out) ;
	if (st == UGLY_OK){
		if ((method == NULL)||(out == NULL)){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_invoke_opts: method and out are required") ;
		}
		else if (c.p->invoke_opts == NULL){
			st = no_opts(&c) ;
		}
		else if ((st = borrow_opts(&c, opts, &o, &kw)) == UGLY_OK){
			st = c.p->invoke_opts(ctx, c.rt, c.ref, method, c.bargs, nargs, &o, want, out) ;
		}
	}
	free(kw) ;
	return end(&c, st, "invoke", want, out) ;
}


ugly_status ugly_new_opts(ugly_ctx *ctx, ugly_handle cls, const ugly_val *args, size_t nargs,
	const ugly_opts *opts, ugly_handle *out){

	if (! has_opts(opts)){
		return ugly_new(ctx, cls, args, nargs, out) ;
	}
	call c ;
	ugly_kwarg *kw = NULL ;
	ugly_opts o ;
	ugly_val v = ugly_null() ;
	if (out != NULL){
		*out = UGLY_NO_HANDLE ;
	}
	ugly_status st = begin(&c, ctx, cls, args, nargs, NULL) ;
	if (st == UGLY_OK){
		if (out == NULL){
			st = ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_new_opts: out is required") ;
		}
		else if (c.p->new_opts == NULL){
			st = no_opts(&c) ;
		}
		else if ((st = borrow_opts(&c, opts, &o, &kw)) == UGLY_OK){
			ugly_handle h = UGLY_NO_HANDLE ;
			st = c.p->new_opts(ctx, c.rt, c.ref, c.bargs, nargs, &o, &h) ;
			if (st == UGLY_OK){
				v = ugly_hval(h) ;
			}
		}
	}
	free(kw) ;
	st = end(&c, st, "new", UGLY_HANDLE, &v) ;
	if (st == UGLY_OK){
		*out = v.as.h ;
	}
	return st ;
}


/* ---------------------------------------------------------------- */
/* Ownership                                                        */
/* ---------------------------------------------------------------- */

ugly_status ugly_dup(ugly_ctx *ctx, ugly_handle h, ugly_handle *out){
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	if (out == NULL){
		return ctx_fail(ctx, UGLY_ERR_USAGE, "core", "", "ugly_dup: out is required") ;
	}
	*out = UGLY_NO_HANDLE ;
	if (h_dup(h, out) != UGLY_OK){
		return ctx_fail(ctx, UGLY_ERR_STALE_HANDLE, "core", "", "stale or invalid handle") ;
	}
	return UGLY_OK ;
}


/* Doesn't touch ctx's error: cleanup after a failure must not erase it. */
void ugly_release(ugly_ctx *ctx, ugly_handle h){
	(void)ctx ;
	if (h != UGLY_NO_HANDLE){
		h_release(h) ;
	}
}


ugly_status ugly_same(ugly_ctx *ctx, ugly_handle a, ugly_handle b, bool *out){
	ugly_status st = ctx_check(ctx) ;
	if (st != UGLY_OK){
		return st ;
	}
	int same = 0 ;
	if ((out == NULL)||(h_same(a, b, &same) != UGLY_OK)){
		return ctx_fail(ctx, (out == NULL ? UGLY_ERR_USAGE : UGLY_ERR_STALE_HANDLE), "core", "",
			"ugly_same: invalid arguments") ;
	}
	*out = (same != 0) ;
	return UGLY_OK ;
}


/* ---------------------------------------------------------------- */
/* Plugin services                                                  */
/* ---------------------------------------------------------------- */

ugly_status ugly_export(ugly_ctx *ctx, ugly_rt *rt, void *ref, uintptr_t identity, ugly_handle *out){
	(void)ctx ;
	int duplicate = 0 ;
	ugly_status st = h_export(rt, ref, identity, out, &duplicate) ;
	if ((st != UGLY_OK)||(duplicate)){
		/* The plugin's extra native reference isn't needed (or can't be kept). */
		rt->lang->plugin->release(rt, ref) ;
	}
	if (st != UGLY_OK){
		*out = UGLY_NO_HANDLE ;
	}
	return st ;
}


ugly_status ugly_resolve(ugly_ctx *ctx, ugly_rt *rt, ugly_handle h, void **ref){
	(void)ctx ;
	ugly_rt *home ;
	void *r ;
	*ref = NULL ;
	ugly_status st = h_lookup(h, &home, &r) ;
	if ((st == UGLY_OK)&&(home == rt)){
		*ref = r ;
	}
	return st ;
}
