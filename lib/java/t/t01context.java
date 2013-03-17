import ugly.UglyContext ;
import ugly.UglyError ;

class t01context {
	public static void main(String[] args) {
		JTap jtap = new JTap() ;

		jtap.plan_tests(9) ;
		UglyContext ctx = new UglyContext() ;
		jtap.ok(! ctx.hasError(), "Context doesn't have a pending error") ;
		jtap.ok(ctx.getError() == UglyError.OK, "Context error is UGLY_OK") ;
		jtap.ok(ctx.getErrorMsg() == null, "Context error msg is NULL") ;

		ctx.setError(UglyError.INTERNAL_ERROR, null) ;
		jtap.ok(ctx.getError() == UglyError.INTERNAL_ERROR, "Context error is UGLY_INTERNAL_ERROR") ;
		jtap.ok(ctx.getErrorMsg() == null, "Context error msg is NULL") ;

		ctx.clearError() ;
		jtap.ok(ctx.getError() == UglyError.OK, "Context error is UGLY_OK") ;
		jtap.ok(ctx.getErrorMsg() == null, "Context error msg is NULL") ;

		ctx.setError(UglyError.INTERNAL_ERROR, "internal error") ;
		jtap.ok(ctx.getError() == UglyError.INTERNAL_ERROR, "Context error is UGLY_INTERNAL_ERROR") ;
		jtap.ok(ctx.getErrorMsg().equals("internal error"), "Context error msg is \"internal error\"") ;
		jtap.diag(ctx.getErrorMsg()) ;

		ctx.delete() ;
	}
}

