plan_tests(3) ;

ugly_context *ctx = ugly_context_new() ;

ugly_language *lang = ugly_language_init(ctx, UGLY_TEST_LANG) ; /* UGLY_TEST_OPTS */ 
UGLY_CHECK_ERROR(ctx, 3){
	ok(lang != NULL, "Language %s initialized", UGLY_TEST_LANG) ;
	const char *name = ugly_language_get_name(lang) ;
	ok(strcmp(UGLY_TEST_LANG, name) == 0, "Language name is %s", name) ;
	const char *dir = ugly_language_get_dir(lang) ;
	ok(dir != NULL, "Language directory is %s", dir) ;
}

ugly_context_delete(ctx) ; 
