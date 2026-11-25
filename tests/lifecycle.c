/* Lifecycle: capabilities, loading, runtime deletion, restart (§7). */
#include "conformance.h"


/* A callback that tries to delete the runtime it is being called from. */
static ugly_rt *victim ;
static ugly_status delete_rt(ugly_ctx *ctx, void *data, const ugly_val *args, size_t n, ugly_kind want, ugly_val *out){
	(void)data ; (void)args ; (void)n ; (void)want ;
	*out = ugly_null() ;
	ugly_status st = ugly_rt_delete(ctx, victim) ;
	*out = ugly_int(st) ;
	return UGLY_OK ;
}


int main(void){
	fixture f ;
	fx_setup(&f) ;
	ugly_ctx *ctx = f.ctx ;
	ugly_val v ;
	ugly_status st ;
	uint32_t caps = ugly_lang_caps(f.lang) ;

	/* Loading. */
	ugly_lang *l2 ;
	st = ugly_lang_load(ctx, plugin_name(), &l2) ;
	ok((st == UGLY_OK)&&(l2 == f.lang), "loading a loaded language returns the same ugly_lang") ;
	st = ugly_lang_unload(ctx, l2) ;
	is_status(ctx, st, UGLY_OK, "  ... and unloading it again is fine while another load remains") ;
	is_str(ugly_lang_name(f.lang), plugin_name(), "ugly_lang_name") ;
	st = ugly_lang_load(ctx, "no_such_language", &l2) ;
	is_status(ctx, st, UGLY_ERR_NOT_FOUND, "unknown language: UGLY_ERR_NOT_FOUND") ;
	st = ugly_lang_load(ctx, "../etc", &l2) ;
	is_status(ctx, st, UGLY_ERR_USAGE, "a name that isn't a plain identifier: UGLY_ERR_USAGE") ;
	st = ugly_lang_unload(ctx, f.lang) ;
	is_status(ctx, st, UGLY_ERR_IN_USE, "unload with a live runtime: UGLY_ERR_IN_USE") ;

	/* Options: set before a language loads. */
	st = ugly_configure(ctx, plugin_name(), "path", "/nowhere") ;
	is_status(ctx, st, UGLY_ERR_IN_USE, "ugly_configure on a loaded language: UGLY_ERR_IN_USE") ;
	st = ugly_configure(ctx, "c", "path", "/nowhere") ;
	is_status(ctx, st, UGLY_ERR_IN_USE, "ugly_configure on the built-in c language: UGLY_ERR_IN_USE") ;
	st = ugly_configure(ctx, "../etc", "path", "/nowhere") ;
	is_status(ctx, st, UGLY_ERR_USAGE, "ugly_configure with an invalid language name: UGLY_ERR_USAGE") ;

	/* Optional capabilities fail cleanly when absent. */
	if (! (caps & UGLY_CAP_EVAL)){
		st = ugly_rt_eval(ctx, f.rt, "1", 1, UGLY_ANY, &v) ;
		is_status(ctx, st, UGLY_ERR_NOT_SUPPORTED, "eval without UGLY_CAP_EVAL: UGLY_ERR_NOT_SUPPORTED") ;
	}
	else {
		skip("plugin has UGLY_CAP_EVAL") ;
	}
	if (! (caps & UGLY_CAP_ATTACH)){
		ugly_rt *rt ;
		st = ugly_rt_attach(ctx, f.lang, NULL, &rt) ;
		is_status(ctx, st, UGLY_ERR_NOT_SUPPORTED, "attach without UGLY_CAP_ATTACH: UGLY_ERR_NOT_SUPPORTED") ;
	}
	else {
		skip("plugin has UGLY_CAP_ATTACH") ;
	}

	/* A runtime can't be deleted from inside a call into it. */
	ugly_handle cb ;
	ugly_callable_new(ctx, delete_rt, NULL, NULL, &cb) ;
	victim = f.rt ;
	ugly_val a[] = { ugly_hval(cb), ugly_null() } ;
	st = fcall(&f, f.mod, "apply", a, 2, UGLY_INT, &v) ;
	ok((st == UGLY_OK)&&(v.as.i == UGLY_ERR_IN_USE), "deleting a runtime during a call into it: UGLY_ERR_IN_USE") ;
	ugly_release(ctx, cb) ;

	/* Deleting a runtime invalidates its handles, wherever they are held. */
	if (caps & UGLY_CAP_MULTI_RUNTIME){
		ugly_rt *rt2 ;
		ugly_handle mod2 ;
		if (fx_runtime(&f, &rt2, &mod2)){
			ugly_handle c2 = make_counter(&f, mod2) ;
			ugly_handle held ;
			ugly_val one = ugly_hval(c2) ;
			ugly_list_new(ctx, &one, 1, &held) ;           /* another runtime keeps a reference */
			st = ugly_rt_delete(ctx, rt2) ;
			is_status(ctx, st, UGLY_OK, "deleting a runtime whose values are still referenced succeeds") ;
			st = ugly_invoke(ctx, c2, "inc", NULL, 0, UGLY_ANY, &v) ;
			is_status(ctx, st, UGLY_ERR_STALE_HANDLE, "  ... and its handles are stale") ;
			st = ugly_get_index(ctx, held, 0, UGLY_HANDLE, &v) ;
			if (st == UGLY_OK){
				st = ugly_invoke(ctx, v.as.h, "inc", NULL, 0, UGLY_ANY, &v) ;
			}
			is_status(ctx, st, UGLY_ERR_STALE_HANDLE, "  ... including references held by other runtimes") ;
			ugly_release(ctx, c2) ;
			ugly_release(ctx, mod2) ;
			ugly_release(ctx, held) ;
		}
	}
	else {
		ugly_rt *rt2 ;
		st = ugly_rt_new(ctx, f.lang, &rt2) ;
		is_status(ctx, st, UGLY_ERR_NOT_SUPPORTED, "a second runtime without UGLY_CAP_MULTI_RUNTIME: UGLY_ERR_NOT_SUPPORTED") ;
	}

	/* Restart: unload completely, load again, and it works (only if the plugin says it can). */
	if (caps & UGLY_CAP_RESTARTABLE){
		/* inits() is an optional fixture: how often the plugin's process-wide init ran. */
		ugly_val n1 = ugly_int(-1) ;
		int counted = (fcall(&f, f.mod, "inits", NULL, 0, UGLY_INT, &n1) == UGLY_OK) ;
		ugly_release(ctx, f.mod) ;
		ugly_rt_delete(ctx, f.rt) ;
		st = ugly_lang_unload(ctx, f.lang) ;
		is_status(ctx, st, UGLY_OK, "unload once the last runtime is gone") ;
		/* Options again: only if the plugin was torn down (a plugin without 'term' stays initialized). */
		st = ugly_configure(ctx, plugin_name(), "no_such_option", "x") ;
		ok((st == UGLY_OK)||(st == UGLY_ERR_IN_USE), "ugly_configure after unload: OK, or UGLY_ERR_IN_USE if the plugin stays initialized") ;
		if (st == UGLY_OK){
			st = ugly_lang_load(ctx, plugin_name(), &l2) ;
			is_status(ctx, st, UGLY_ERR_USAGE, "  ... an option the plugin doesn't take fails the load: UGLY_ERR_USAGE") ;
			st = ugly_configure(ctx, plugin_name(), "no_such_option", NULL) ;
			is_status(ctx, st, UGLY_OK, "  ... and a NULL value clears it") ;
		}
		else {
			skip("the plugin stays initialized after unloading") ;
		}
		st = ugly_lang_load(ctx, plugin_name(), &f.lang) ;
		is_status(ctx, st, UGLY_OK, "load again") ;
		ok(fx_runtime(&f, &f.rt, &f.mod), "  ... and create a runtime") ;
		if (counted){
			ugly_val n2 ;
			st = fcall(&f, f.mod, "inits", NULL, 0, UGLY_INT, &n2) ;
			if (! ok((st == UGLY_OK)&&(n2.as.i == n1.as.i + 1), "  ... the plugin was initialized again")){
				diag("  inits before %lld, after %lld", (long long)n1.as.i, (long long)n2.as.i) ;
			}
		}
		else {
			skip("the plugin has no inits() fixture") ;
		}
	}
	else {
		skip("plugin lacks UGLY_CAP_RESTARTABLE") ;
	}

	fx_teardown(&f) ;
	return done_testing() ;
}
