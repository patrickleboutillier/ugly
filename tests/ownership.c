/* Ownership: release runs finalizers, dup keeps alive, stale and kept-borrow handles are detected (§3). */
#include "conformance.h"


/* A callback that (wrongly) keeps the borrowed handle it was given. */
static ugly_handle kept ;
static ugly_status keep_borrow(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)ctx ; (void)data ; (void)want ;
	kept = (n > 0 ? args[0].as.h : UGLY_NO_HANDLE) ;
	*out = ugly_null() ;
	return UGLY_OK ;
}

/* A callback that keeps it correctly, with ugly_dup. */
static ugly_status keep_dup(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)data ; (void)want ;
	*out = ugly_null() ;
	return (n > 0 ? ugly_dup(ctx, args[0].as.h, &kept) : UGLY_ERR_USAGE) ;
}


static int freed_data ;
static void free_data(void *p){
	(void)p ;
	freed_data++ ;
}


static ugly_status noop(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)ctx ; (void)data ; (void)args ; (void)n ; (void)want ;
	*out = ugly_null() ;
	return UGLY_OK ;
}


static ugly_status apply(fixture *f, ugly_handle fn, ugly_handle arg){
	ugly_val args[] = { ugly_hval(fn), ugly_hval(arg) } ;
	ugly_val out ;
	ugly_status st = fcall(f, f->mod, "apply", args, 2, UGLY_ANY, &out) ;
	ugly_val_release(f->ctx, &out) ;
	return st ;
}


int main(void){
	fixture f ;
	fx_setup(&f) ;
	ugly_ctx *ctx = f.ctx ;
	ugly_val v ;
	ugly_status st ;

	/* Releasing the last reference frees the object in its home runtime. */
	int64_t before = finalized(&f, f.mod) ;
	ugly_handle c = make_counter(&f, f.mod) ;
	ok(c != UGLY_NO_HANDLE, "make_counter") ;
	is_int(finalized(&f, f.mod), before, "  ... nothing finalized while referenced") ;
	ugly_release(ctx, c) ;
	is_int(finalized(&f, f.mod), before + 2, "release: the counter and its items are finalized") ;

	st = ugly_invoke(ctx, c, "inc", NULL, 0, UGLY_ANY, &v) ;
	is_status(ctx, st, UGLY_ERR_STALE_HANDLE, "using a released handle: UGLY_ERR_STALE_HANDLE") ;

	/* dup keeps the object alive past the original's release. */
	before = finalized(&f, f.mod) ;
	c = make_counter(&f, f.mod) ;
	ugly_handle d ;
	st = ugly_dup(ctx, c, &d) ;
	is_status(ctx, st, UGLY_OK, "dup") ;
	ugly_release(ctx, c) ;
	st = ugly_invoke(ctx, d, "inc", NULL, 0, UGLY_INT, &v) ;
	is_status(ctx, st, UGLY_OK, "the dup still works after the original is released") ;
	is_int(finalized(&f, f.mod), before, "  ... and nothing was finalized") ;
	ugly_release(ctx, d) ;
	is_int(finalized(&f, f.mod), before + 2, "releasing the dup finalizes it") ;

	/* Borrows end with their call, and keeping one is detected, not undefined. */
	ugly_handle cb ;
	ugly_callable_new(ctx, keep_borrow, NULL, NULL, &cb) ;
	c = make_counter(&f, f.mod) ;
	st = apply(&f, cb, c) ;
	is_status(ctx, st, UGLY_OK, "apply(callback, counter): the callback keeps its borrowed argument") ;
	ok(kept != UGLY_NO_HANDLE, "  ... it received a handle") ;
	st = ugly_invoke(ctx, kept, "inc", NULL, 0, UGLY_ANY, &v) ;
	is_status(ctx, st, UGLY_ERR_STALE_HANDLE, "using the kept borrow after the call: UGLY_ERR_STALE_HANDLE") ;
	ugly_release(ctx, cb) ;

	ugly_callable_new(ctx, keep_dup, NULL, NULL, &cb) ;
	st = apply(&f, cb, c) ;
	is_status(ctx, st, UGLY_OK, "apply(callback, counter): the callback dups its argument") ;
	st = ugly_invoke(ctx, kept, "inc", NULL, 0, UGLY_INT, &v) ;
	is_status(ctx, st, UGLY_OK, "  ... and the dup works after the call") ;
	bool same = false ;
	ugly_same(ctx, kept, c, &same) ;
	ok(same, "  ... and is the same value") ;
	ugly_release(ctx, kept) ;
	ugly_release(ctx, cb) ;
	ugly_release(ctx, c) ;

	/* A callable's data is freed with its last reference. */
	freed_data = 0 ;
	ugly_callable_new(ctx, noop, &freed_data, free_data, &cb) ;
	st = ugly_dup(ctx, cb, &d) ;
	ugly_release(ctx, cb) ;
	is_int(freed_data, 0, "callable data kept while a reference remains") ;
	ugly_release(ctx, d) ;
	is_int(freed_data, 1, "callable data freed with the last reference") ;

	fx_teardown(&f) ;
	return done_testing() ;
}
