/* Runtimes: load a library and call functions in it. Uses test/<lang>/calc. */
#include "ugly_test.h"


int main(void){
	const char *name = ugly_test_lang() ;
	ugly_context *ctx = ugly_context_new() ;

	ugly_language *lang = ugly_language_init(ctx, name) ;
	if (! no_error(ctx, "language %s initialized", name)){
		return done_testing() ;
	}

	ugly_runtime *rt = ugly_runtime_new(ctx, lang) ;
	if (! no_error(ctx, "%s runtime created", name)){
		return done_testing() ;
	}
	ok(ugly_runtime_get_language(rt) == lang, "runtime belongs to its language") ;

	ugly_runtime_load_library(ctx, rt, "calc") ;
	if (no_error(ctx, "library calc loaded")){
		{
			ugly_value *args[] = { ugly_value_new_long(1), ugly_value_new_long(2) } ;
			ugly_value *v = ugly_runtime_call_function(ctx, rt, UGLY_LONG, "calc::add", args, 2, NULL) ;
			if (no_error(ctx, "calc::add(1, 2) called")){
				ok(v != NULL, "add(1, 2): value returned") ;
				is_long(ugly_value_get_type(v), UGLY_LONG, "add(1, 2): value is a long") ;
				is_long(ugly_value_get_long(v), 3, "add(1, 2) == 3") ;
				ugly_value_delete(v) ;
			}
			ugly_value_delete(args[0]) ;
			ugly_value_delete(args[1]) ;
		}

		{
			ugly_value *args[] = { ugly_value_new_double(1.2), ugly_value_new_double(3.4) } ;
			ugly_value *v = ugly_runtime_call_function(ctx, rt, UGLY_DOUBLE, "calc::add", args, 2, NULL) ;
			if (no_error(ctx, "calc::add(1.2, 3.4) called")){
				is_long(ugly_value_get_type(v), UGLY_DOUBLE, "add(1.2, 3.4): value is a double") ;
				is_double(ugly_value_get_double(v), 4.6, "add(1.2, 3.4) == 4.6") ;
				ugly_value_delete(v) ;
			}
			ugly_value_delete(args[0]) ;
			ugly_value_delete(args[1]) ;
		}

		{
			/* The caller chooses the return type: the same result, requested as a string. */
			ugly_value *args[] = { ugly_value_new_long(6), ugly_value_new_long(7) } ;
			ugly_value *v = ugly_runtime_call_function(ctx, rt, UGLY_STRING, "calc::multiply", args, 2, NULL) ;
			if (no_error(ctx, "calc::multiply(6, 7) called, requesting a string")){
				is_long(ugly_value_get_type(v), UGLY_STRING, "multiply(6, 7): value is a string") ;
				is_string(ugly_value_get_string(v), "42", "multiply(6, 7) == \"42\"") ;
				ugly_value_delete(v) ;
			}
			ugly_value_delete(args[0]) ;
			ugly_value_delete(args[1]) ;
		}

		{
			ugly_value *v = ugly_runtime_call_function(ctx, rt, UGLY_LONG, "calc::no_such_function", NULL, 0, NULL) ;
			ok(v == NULL, "undefined function: no value returned") ;
			has_error(ctx, UGLY_RUNTIME_ERROR, "undefined function: UGLY_RUNTIME_ERROR") ;
		}

		{
			/* The runtime is still usable after an error. */
			ugly_value *args[] = { ugly_value_new_long(10), ugly_value_new_long(4) } ;
			ugly_value *v = ugly_runtime_call_function(ctx, rt, UGLY_LONG, "calc::subtract", args, 2, NULL) ;
			if (no_error(ctx, "calc::subtract(10, 4) called after an error")){
				is_long(ugly_value_get_long(v), 6, "subtract(10, 4) == 6") ;
				ugly_value_delete(v) ;
			}
			ugly_value_delete(args[0]) ;
			ugly_value_delete(args[1]) ;
		}
	}

	ugly_runtime_load_library(ctx, rt, "no_such_library") ;
	has_error(ctx, UGLY_RUNTIME_ERROR, "missing library: UGLY_RUNTIME_ERROR") ;

	/* Two runtimes of the same language in one process, called alternately. */
	ugly_runtime *rt2 = ugly_runtime_new(ctx, lang) ;
	if (no_error(ctx, "second %s runtime created", name)){
		ok(rt2 != rt, "second runtime is distinct") ;
		ugly_runtime_load_library(ctx, rt2, "calc") ;
		if (no_error(ctx, "library calc loaded in second runtime")){
			int i ;
			int good = 1 ;
			for (i = 0 ; i < 4 ; i++){
				ugly_runtime *r = (i % 2 == 0 ? rt : rt2) ;
				ugly_value *args[] = { ugly_value_new_long(i), ugly_value_new_long(100) } ;
				ugly_value *v = ugly_runtime_call_function(ctx, r, UGLY_LONG, "calc::add", args, 2, NULL) ;
				if ((v == NULL)||(ugly_value_get_long(v) != i + 100)){
					good = 0 ;
					diag("  call %d on runtime %d: %s", i, (i % 2) + 1,
						ugly_context_has_error(ctx) ? ugly_context_get_error_msg(ctx) : "wrong value") ;
				}
				if (v != NULL){
					ugly_value_delete(v) ;
				}
				ugly_value_delete(args[0]) ;
				ugly_value_delete(args[1]) ;
			}
			ok(good, "alternating calls between two runtimes") ;
		}
	}

	ugly_context_delete(ctx) ;
	return done_testing() ;
}
