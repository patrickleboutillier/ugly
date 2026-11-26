/*
 * Call options (§11): keyword arguments, a call context, an overload hint.
 * Each is honoured with the callee's language's own meaning, or the call is
 * refused with UGLY_ERR_NOT_SUPPORTED: exact or refused, never dropped.
 *
 * Fixtures: kw(a, b=0, c=0) returns "a b c"; KwBox(value=0), with
 * box.add(n=1) = value + n; context() returns the context it was called in
 * (Perl only: others refuse a context before looking it up).
 */
#include "conformance.h"


static ugly_status invoke_kw(fixture *f, ugly_handle obj, const char *method, const ugly_val *args, size_t n,
	const ugly_kwarg *kw, size_t nkw, ugly_kind want, ugly_val *out){

	ugly_opts o = UGLY_OPTS_INIT ;
	o.kwargs = kw ;
	o.nkwargs = nkw ;
	return ugly_invoke_opts(f->ctx, obj, method, args, n, &o, want, out) ;
}


int main(void){
	fixture f ;
	fx_setup(&f) ;
	ugly_ctx *ctx = f.ctx ;
	ugly_val v, one = ugly_int(1) ;
	ugly_status st ;

	/* Keyword arguments. */
	ugly_kwarg cb[] = { { "c", ugly_int(3) }, { "b", ugly_int(2) } } ;
	st = invoke_kw(&f, f.mod, "kw", &one, 1, cb, 2, UGLY_STRING, &v) ;
	int kwargs = (st == UGLY_OK) ;
	if (kwargs){
		is_str(sval(&v), "1 2 3", "keyword arguments reach the callee by name: invoke kw(1, c=3, b=2)") ;
		ugly_val_release(ctx, &v) ;

		ugly_val fn ;
		ugly_kwarg c[] = { { "c", ugly_int(3) } } ;
		ugly_opts o = UGLY_OPTS_INIT ;
		o.kwargs = c ;
		o.nkwargs = 1 ;
		st = ugly_get_member(ctx, f.mod, "kw", UGLY_HANDLE, &fn) ;
		if (st == UGLY_OK){
			st = ugly_call_opts(ctx, fn.as.h, &one, 1, &o, UGLY_STRING, &v) ;
			if (is_status(ctx, st, UGLY_OK, "call kw(1, c=3)")){
				is_str(sval(&v), "1 0 3", "  ... the others keep their defaults") ;
				ugly_val_release(ctx, &v) ;
			}
			ugly_opts none = UGLY_OPTS_INIT ;
			st = ugly_call_opts(ctx, fn.as.h, &one, 1, &none, UGLY_STRING, &v) ;
			if (is_status(ctx, st, UGLY_OK, "empty call options are a plain call")){
				is_str(sval(&v), "1 0 0", "  ... kw(1)") ;
				ugly_val_release(ctx, &v) ;
			}
			ugly_val_release(ctx, &fn) ;
		}

		/* new, and invoke on the instance. */
		ugly_val cls ;
		st = ugly_get_member(ctx, f.mod, "KwBox", UGLY_HANDLE, &cls) ;
		if (is_status(ctx, st, UGLY_OK, "get_member(module, KwBox)")){
			ugly_kwarg value[] = { { "value", ugly_int(5) } } ;
			ugly_opts no = UGLY_OPTS_INIT ;
			no.kwargs = value ;
			no.nkwargs = 1 ;
			ugly_handle box ;
			st = ugly_new_opts(ctx, cls.as.h, NULL, 0, &no, &box) ;
			if (is_status(ctx, st, UGLY_OK, "new KwBox(value=5)")){
				st = ugly_get_member(ctx, box, "value", UGLY_INT, &v) ;
				ok((st == UGLY_OK)&&(v.as.i == 5), "  ... its value is 5") ;
				ugly_kwarg n[] = { { "n", ugly_int(10) } } ;
				st = invoke_kw(&f, box, "add", NULL, 0, n, 1, UGLY_INT, &v) ;
				ok((st == UGLY_OK)&&(v.as.i == 15), "invoke box.add(n=10) on an instance: 15") ;
				st = ugly_invoke(ctx, box, "add", NULL, 0, UGLY_INT, &v) ;
				ok((st == UGLY_OK)&&(v.as.i == 6), "  ... and box.add() keeps its default: 6") ;
				ugly_release(ctx, box) ;
			}
			ugly_val_release(ctx, &cls) ;
		}
	}
	else {
		is_status(ctx, st, UGLY_ERR_NOT_SUPPORTED, "keyword arguments: refused (UGLY_ERR_NOT_SUPPORTED)") ;
		skip("the plugin refuses keyword arguments: call, new, invoke on an instance") ;
	}

	/* A call context: Perl's scalar context, or refused. */
	ugly_opts sc = UGLY_OPTS_INIT ;
	sc.context = "scalar" ;
	st = ugly_invoke_opts(ctx, f.mod, "context", NULL, 0, &sc, UGLY_STRING, &v) ;
	if (st == UGLY_OK){
		is_str(sval(&v), "scalar", "context \"scalar\": the callee runs in scalar context") ;
		ugly_val_release(ctx, &v) ;
		ugly_opts li = UGLY_OPTS_INIT ;
		li.context = "list" ;
		st = ugly_invoke_opts(ctx, f.mod, "context", NULL, 0, &li, UGLY_STRING, &v) ;
		ok((st == UGLY_OK)&&(strcmp(sval(&v), "list") == 0), "  ... and \"list\" in list context") ;
		ugly_val_release(ctx, &v) ;
		li.context = "sideways" ;
		st = ugly_invoke_opts(ctx, f.mod, "context", NULL, 0, &li, UGLY_STRING, &v) ;
		is_status(ctx, st, UGLY_ERR_USAGE, "  ... an unknown context: UGLY_ERR_USAGE") ;
	}
	else {
		is_status(ctx, st, UGLY_ERR_NOT_SUPPORTED, "a call context: refused (UGLY_ERR_NOT_SUPPORTED)") ;
	}

	/*
	 * An overload hint: Java and C# pick the overload (over(int), over(long),
	 * over(Object)), by their own names for the parameter types; Haskell
	 * picks the instantiation of over :: Over a => a -> String, by type
	 * application; others refuse it.
	 */
	static const char *const java_hints[][2] = {
		{ NULL, "long" }, { "int", "int" }, { "long", "long" }, { "java.lang.Object", "java.lang.Object" }, { NULL, NULL },
	} ;
	static const char *const dotnet_hints[][2] = {
		{ NULL, "long" }, { "int", "int" }, { "long", "long" }, { "object", "object" }, { "System.Int32", "int" }, { NULL, NULL },
	} ;
	static const char *const haskell_hints[][2] = {
		{ NULL, "Int" }, { "Int", "Int" }, { "@Int64", "Int64" }, { "Value", "Value" }, { NULL, NULL },
	} ;
	const char *const (*hints)[2] = (strcmp(plugin_name(), "java") == 0 ? java_hints
		: strcmp(plugin_name(), "dotnet") == 0 ? dotnet_hints
		: strcmp(plugin_name(), "haskell") == 0 ? haskell_hints : NULL) ;
	ugly_opts h = UGLY_OPTS_INIT ;
	h.hint = "int" ;
	if (hints == NULL){
		st = ugly_invoke_opts(ctx, f.mod, "echo", &one, 1, &h, UGLY_ANY, &v) ;
		is_status(ctx, st, UGLY_ERR_NOT_SUPPORTED, "an overload hint: refused (UGLY_ERR_NOT_SUPPORTED)") ;
	}
	else {
		size_t i ;
		for (i = 0 ; hints[i][1] != NULL ; i++){
			h.hint = hints[i][0] ;
			char what[96] ;
			snprintf(what, sizeof(what), "over(1) with hint %s", hints[i][0] ? hints[i][0] : "(none): the best fit") ;
			st = ugly_invoke_opts(ctx, f.mod, "over", &one, 1, &h, UGLY_STRING, &v) ;
			if (is_status(ctx, st, UGLY_OK, what)){
				snprintf(what, sizeof(what), "  ... calls over(%s)", hints[i][1]) ;
				is_str(sval(&v), hints[i][1], what) ;
				ugly_val_release(ctx, &v) ;
			}
		}
		h.hint = "double" ;
		st = ugly_invoke_opts(ctx, f.mod, "over", &one, 1, &h, UGLY_STRING, &v) ;
		is_status(ctx, st, UGLY_ERR_NOT_FOUND, "a hint naming no overload: UGLY_ERR_NOT_FOUND") ;
		h.hint = "int" ;
		st = ugly_invoke_opts(ctx, f.mod, "over", &one, 1, &h, UGLY_STRING, &v) ;
		if (st == UGLY_OK){
			ugly_val_release(ctx, &v) ;
		}
		st = ugly_invoke(ctx, f.mod, "over", &one, 1, UGLY_STRING, &v) ;
		ok((st == UGLY_OK)&&(strcmp(sval(&v), hints[0][1]) == 0), "  ... and a hint lasts one call: the next one picks the best fit again") ;
		if (st == UGLY_OK){
			ugly_val_release(ctx, &v) ;
		}
	}

	/* An options struct from an older caller (smaller 'size'): what it has still counts. */
	ugly_opts old = UGLY_OPTS_INIT ;
	old.size = offsetof(ugly_opts, hint) ;
	old.kwargs = cb ;
	old.nkwargs = 2 ;
	old.hint = "ignored: past this caller's struct" ;
	st = ugly_invoke_opts(ctx, f.mod, "kw", &one, 1, &old, UGLY_STRING, &v) ;
	is_status(ctx, st, kwargs ? UGLY_OK : UGLY_ERR_NOT_SUPPORTED, "a smaller ugly_opts (an older caller): only its fields count") ;
	if (st == UGLY_OK){
		ugly_val_release(ctx, &v) ;
	}

	fx_teardown(&f) ;
	return done_testing() ;
}
