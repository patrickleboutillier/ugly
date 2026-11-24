/* Objects: create a foreign object, call methods on it, delete it. Uses test/<lang>/oocalc. */
#include "ugly_test.h"


static void check_add(ugly_context *ctx, ugly_object *o, long a, long b){
	ugly_value *args[] = { ugly_value_new_long(a), ugly_value_new_long(b) } ;
	ugly_value *v = ugly_object_call_method(ctx, o, UGLY_LONG, "add", args, 2, NULL) ;
	if (no_error(ctx, "oocalc->add(%ld, %ld) called", a, b)){
		is_long(ugly_value_get_type(v), UGLY_LONG, "add(%ld, %ld): value is a long", a, b) ;
		is_long(ugly_value_get_long(v), a + b, "add(%ld, %ld) == %ld", a, b, a + b) ;
		ugly_value_delete(v) ;
	}
	ugly_value_delete(args[0]) ;
	ugly_value_delete(args[1]) ;
}


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

	ugly_runtime_load_library(ctx, rt, "oocalc") ;
	if (! no_error(ctx, "library oocalc loaded")){
		return done_testing() ;
	}

	ugly_object *o = ugly_object_new(ctx, rt, "oocalc", NULL, 0, NULL) ;
	if (no_error(ctx, "oocalc->new() called")){
		ok(o != NULL, "object returned: %s", o ? ugly_object_to_string(o) : "NULL") ;
		ok(ugly_object_get_runtime(o) == rt, "object belongs to its runtime") ;
		is_string(ugly_object_get_class(o), "oocalc", "object class") ;

		/* Several calls on the same object: the handle must survive each call. */
		check_add(ctx, o, 1, 2) ;
		check_add(ctx, o, 40, 2) ;
		check_add(ctx, o, -5, 5) ;

		ugly_value *v = ugly_object_call_method(ctx, o, UGLY_LONG, "no_such_method", NULL, 0, NULL) ;
		ok(v == NULL, "undefined method: no value returned") ;
		has_error(ctx, UGLY_RUNTIME_ERROR, "undefined method: UGLY_RUNTIME_ERROR") ;

		/* A runtime can't go while its objects are alive. */
		ugly_runtime_delete(ctx, rt) ;
		has_error(ctx, UGLY_IN_USE, "runtime with a live object: delete refused with UGLY_IN_USE") ;
		check_add(ctx, o, 7, 8) ;

		ugly_object_delete(ctx, o) ;
		no_error(ctx, "object deleted") ;

		/* Deleting the handle must release the foreign object, not just the wrapper. */
		ugly_value *d = ugly_runtime_call_function(ctx, rt, UGLY_LONG, "oocalc::destroyed", NULL, 0, NULL) ;
		if (no_error(ctx, "oocalc::destroyed() called")){
			is_long(ugly_value_get_long(d), 1, "the foreign object was destroyed when its handle was deleted") ;
			ugly_value_delete(d) ;
		}
	}

	ugly_object_delete(ctx, NULL) ;
	no_error(ctx, "deleting a NULL object is a no-op") ;

	ugly_runtime_delete(ctx, rt) ;
	no_error(ctx, "runtime deleted once its objects are gone") ;
	ugly_language_delete(ctx, lang) ;
	no_error(ctx, "language deleted") ;

	ugly_context_delete(ctx) ;
	return done_testing() ;
}
