/* Identity: the same value maps to the same handle; proxies unwrap when they go home (§3, rules 5 and 6). */
#include "conformance.h"


static int same(fixture *f, ugly_handle a, ugly_handle b){
	bool s = false ;
	return (ugly_same(f->ctx, a, b, &s) == UGLY_OK) && s ;
}


int main(void){
	fixture f ;
	fx_setup(&f) ;
	ugly_ctx *ctx = f.ctx ;
	ugly_status st ;

	/* Exporting the same native object twice gives the same handle. */
	ugly_val e1, e2 ;
	ugly_get_member(ctx, f.mod, "echo", UGLY_HANDLE, &e1) ;
	ugly_get_member(ctx, f.mod, "echo", UGLY_HANDLE, &e2) ;
	ok(same(&f, e1.as.h, e2.as.h), "get_member(module, echo) twice: the same value") ;
	ok(e1.as.h == e2.as.h, "  ... and the same owned handle") ;
	ugly_val_release(ctx, &e1) ;
	ugly_val_release(ctx, &e2) ;

	ugly_handle m2 ;
	st = ugly_rt_import(ctx, f.rt, "conformance", &m2) ;
	ok((st == UGLY_OK)&&(m2 == f.mod), "importing a module twice: the same handle") ;
	ugly_release(ctx, m2) ;

	/* A native object that goes out and comes back is the original. */
	ugly_handle c = make_counter(&f, f.mod) ;
	ugly_val arg = ugly_hval(c), out ;
	st = fcall(&f, f.mod, "echo", &arg, 1, UGLY_HANDLE, &out) ;
	is_status(ctx, st, UGLY_OK, "echo(counter)") ;
	ok(same(&f, out.as.h, c), "  ... returns the same counter") ;
	ugly_val_release(ctx, &out) ;
	ugly_release(ctx, c) ;

	/* A foreign value (a core list) is a proxy inside the language... */
	ugly_val one = ugly_int(1) ;
	ugly_handle l ;
	ugly_list_new(ctx, &one, 1, &l) ;
	arg = ugly_hval(l) ;
	st = fcall(&f, f.mod, "kind_of", &arg, 1, UGLY_STRING, &out) ;
	is_status(ctx, st, UGLY_OK, "kind_of(core list)") ;
	is_str(sval(&out), "proxy", "  ... is a proxy inside the language") ;
	ugly_val_release(ctx, &out) ;

	/* ...and unwraps when it comes back: the original, not a proxy of a proxy. */
	st = fcall(&f, f.mod, "echo", &arg, 1, UGLY_HANDLE, &out) ;
	is_status(ctx, st, UGLY_OK, "echo(core list)") ;
	ok(same(&f, out.as.h, l), "  ... returns the original list") ;
	uint32_t traits = 0 ;
	ugly_val tn ;
	ugly_describe(ctx, out.as.h, &traits, &tn) ;
	is_str(sval(&tn), "list", "  ... still a list, home in the host") ;
	ugly_val_release(ctx, &tn) ;
	ugly_val_release(ctx, &out) ;

	/* The same, through two runtimes: list -> rt2 -> rt1 -> back. */
	ugly_rt *rt2 ;
	ugly_handle mod2 ;
	if (fx_runtime(&f, &rt2, &mod2)){
		ugly_val echo2 ;
		ugly_get_member(ctx, mod2, "echo", UGLY_HANDLE, &echo2) ;
		ugly_val args[] = { echo2, ugly_hval(l) } ;
		st = fcall(&f, f.mod, "apply", args, 2, UGLY_HANDLE, &out) ;
		is_status(ctx, st, UGLY_OK, "rt1.apply(rt2.echo, list)") ;
		ok(same(&f, out.as.h, l), "  ... returns the original list") ;
		ugly_val_release(ctx, &out) ;
		ugly_val_release(ctx, &echo2) ;
		ugly_release(ctx, mod2) ;
		ugly_rt_delete(ctx, rt2) ;
	}
	ugly_release(ctx, l) ;

	fx_teardown(&f) ;
	return done_testing() ;
}
