/*
 * The binding: a Haskell program as the host. This is haskell.c again, in
 * binding mode, compiled by ghc into libugly_hs.so (the package 'ugly'), so
 * a program built with -package ugly has it: Ugly.withUgly registers this
 * plugin as "haskell" (ugly_lang_register: no ugly_haskell.so, whose
 * libugly_rts.so would start a second RTS) and attaches to the program's
 * own RTS. Values the program passes to other languages are then boxes in
 * that runtime, and they can call back into the program.
 */
#define UGLY_BINDING
#include "haskell.c"

extern void ugly_hs_set_attached(ugly_rt *rt) ;        /* glue.c */

static ugly_ctx *b_ctx ;
static ugly_lang *b_lang ;
static ugly_rt *b_rt ;
static int b_owns_ctx, b_depth ;
static char b_error[512] ;

/* Undoes what attach did, on the attaching thread. */
static void binding_teardown(void){
	ugly_hs_set_attached(NULL) ;
	if (b_rt != NULL){
		ugly_rt_delete(b_ctx, b_rt) ;
		b_rt = NULL ;
	}
	if (b_lang != NULL){
		ugly_lang_unload(b_ctx, b_lang) ;
		b_lang = NULL ;
	}
	if (b_owns_ctx){
		ugly_ctx_delete(b_ctx) ;      /* also deletes the runtimes ugly_rt_get made for it */
		b_owns_ctx = 0 ;
	}
	b_ctx = NULL ;
}

/*
 * Ugly.withUgly, on entry: attaches once (nested uses count). Returns
 * UGLY_OK, or a status with ugly_hs_binding_error() saying why.
 */
ugly_status ugly_hs_binding_attach(void){
	if (b_depth++ > 0){
		return UGLY_OK ;
	}
	b_ctx = ugly_current_ctx() ;
	if (b_ctx == NULL){
		ugly_status st = ugly_ctx_new(&b_ctx) ;
		if (st != UGLY_OK){
			snprintf(b_error, sizeof b_error, "can't create a ctx on this thread") ;
			b_depth-- ;
			return st ;
		}
		b_owns_ctx = 1 ;
	}
	ugly_status st = ugly_lang_register(b_ctx, &plugin, &b_lang) ;
	if (st == UGLY_OK){
		st = ugly_rt_attach(b_ctx, b_lang, NULL, &b_rt) ;
	}
	if (st != UGLY_OK){
		const ugly_error *e = ugly_last_error(b_ctx) ;
		snprintf(b_error, sizeof b_error, "%s", e != NULL ? ugly_error_message(e) : "can't attach") ;
		binding_teardown() ;
		b_depth-- ;
		return st ;
	}
	ugly_hs_set_attached(b_rt) ;
	return UGLY_OK ;
}

const char *ugly_hs_binding_error(void){
	return b_error ;
}

/* Ugly.withUgly, on exit: the last one detaches. */
void ugly_hs_binding_detach(void){
	if ((b_depth > 0)&&(--b_depth == 0)){
		binding_teardown() ;
	}
}
