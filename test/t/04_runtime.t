plan_tests(10) ;

ugly_context *ctx = ugly_context_new() ;

ugly_language *lang = ugly_language_init(ctx, UGLY_TEST_LANG) ; /* UGLY_TEST_OPTS */ 
UGLY_CHECK_ERROR(ctx, 1){
    ok(lang != NULL, "Language %s initialized", UGLY_TEST_LANG) ;
}

ugly_runtime *rt = ugly_runtime_new(ctx, lang) ;
UGLY_CHECK_ERROR(ctx, 2){
    ok(rt != NULL, "%s runtime initialized", UGLY_TEST_LANG) ;
    ok(ugly_runtime_get_language(rt) == lang, "Runtime language == language") ;
}

ugly_runtime_load_library(ctx, rt, "calc") ;
UGLY_CHECK_ERROR(ctx, 1){
	pass("Library calc loaded") ;
}

{
ugly_value **args = (ugly_value **)malloc(2 * sizeof(ugly_value *)) ;
args[0] = ugly_value_new_long(1) ;
args[1] = ugly_value_new_long(2) ;
ugly_value *v = ugly_runtime_call_function(ctx, rt, UGLY_LONG, "calc::add", args, 2, NULL) ;
UGLY_CHECK_ERROR(ctx, 3){
	ok(v != NULL, "add(1, 2) result: value returned") ;
	ok(ugly_value_get_type(v) == UGLY_LONG, "add(1, 2) result: value is a long") ;
	ok(ugly_value_get_long(v) == 3, "value is 3") ;
}
free(args) ;
}

{
ugly_value **args = (ugly_value **)malloc(2 * sizeof(ugly_value *)) ;
args[0] = ugly_value_new_double(1.2) ;
args[1] = ugly_value_new_double(3.4) ;
ugly_value *v = ugly_runtime_call_function(ctx, rt, UGLY_DOUBLE, "calc::add", args, 2, NULL) ;
UGLY_CHECK_ERROR(ctx, 3){
	ok(v != NULL, "add(1.2, 3.4) result: value returned") ;
	ok(ugly_value_get_type(v) == UGLY_DOUBLE, "add(1.2, 3.4) result: value is a double") ;
	ok(ugly_value_get_double(v) == 4.6, "value is 4.6") ;
}
free(args) ;
}



ugly_context_delete(ctx) ;
