import ugly.UglyContext ;

class t01context {
	public static void main(String[] args) {
		JTap jtap = new JTap() ;

		jtap.plan_tests(9) ;
		UglyContext ctx = new UglyContext() ;
		jtap.ok(! ctx.hasError(), "Context doesn't have a pending error") ;
	}
}

/*
ok(ugly_context_get_error(ctx) == UGLY_OK, "Context error is UGLY_OK") ;
ok(ugly_context_get_error_msg(ctx) == NULL, "Context error msg is NULL") ;

ugly_context_set_error(ctx, UGLY_INTERNAL_ERROR, NULL) ;
ok(ugly_context_get_error(ctx) == UGLY_INTERNAL_ERROR, "Context error is UGLY_INTERNAL_ERROR") ;
ok(ugly_context_get_error_msg(ctx) == NULL, "Context error msg is NULL") ;

ugly_context_clear_error(ctx) ;
ok(ugly_context_get_error(ctx) == UGLY_OK, "Context error is UGLY_OK") ;
ok(ugly_context_get_error_msg(ctx) == NULL, "Context error msg is NULL") ;

ugly_context_set_error(ctx, UGLY_INTERNAL_ERROR, "internal %s", "error") ;
ok(ugly_context_get_error(ctx) == UGLY_INTERNAL_ERROR, "Context error is UGLY_INTERNAL_ERROR") ;
ok(strcmp(ugly_context_get_error_msg(ctx), "internal error") == 0, "Context error msg is \\"internal error\\"") ;

ugly_context_delete(ctx) ;
*/
