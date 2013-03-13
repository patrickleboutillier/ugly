#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "ugly_perl5_priv.h"


ugly_value *perl_call_helper_function(ugly_context *ctx, ugly_runtime *rt, const char *func, int nb_args, SV **args){
	PerlInterpreter *my_perl = (PerlInterpreter *)rt->handle ;

	dSP ;
	ENTER ;
	SAVETMPS ;
	PUSHMARK(SP) ;

	/* Push ctx and rt objects onto the Perl stack */
	XPUSHs(sv_2mortal(sv_setref_pv(newSVuv(0), "Ugly::Context", (void *)ctx)));
	XPUSHs(sv_2mortal(sv_setref_pv(newSVuv(0), "Ugly::Runtime", (void *)rt)));

	/* Push arguments onto the Perl stack */
    int i = 0 ;
    for (i = 0; i < nb_args ; i++){
		XPUSHs(sv_2mortal(args[i])) ;
	}
	PUTBACK ;

	/* Call perl function */
    int count = call_pv(func, G_SCALAR | G_EVAL) ;

	SPAGAIN ;

	if (SvTRUE(ERRSV)){
		STRLEN n_a ;
		ugly_context_set_error(ctx, UGLY_RUNTIME_ERROR, "Error calling perl5 helper function '%s': %s",
			func, SvPV(ERRSV, n_a)) ;
		return NULL ;
    }
	else if (count != 1){
		ugly_context_set_error(ctx, UGLY_INTERNAL_ERROR, "perl5 helper function must return 1 value (not %d)", count) ;
		return NULL ;
	}

	SV *svret = POPs ;
	ugly_value *ret = NULL ;
	if (SvOK(svret)){
	    if (sv_derived_from(svret, "Ugly::Value")){
			ret = (ugly_value *)SvIV((SV*)SvRV(svret)) ; 
		}
    	else {
			ugly_context_set_error(ctx, UGLY_INTERNAL_ERROR, "perl5 helper function must return undef or an Ugly::Value") ;
			return NULL ;
		}
	}

    FREETMPS ;
    LEAVE ;

    return ret ;
}


ugly_value *perl_call_helper_function_v(ugly_context *ctx, ugly_runtime *rt, const char *func, int nb_args, ...){

    va_list ap ;
    va_start(ap, nb_args) ;

	SV **args = UGLY_MALLOC_N(SV **, nb_args) ;

    int i = 0 ;
    for (i = 0; i < nb_args ; i++){
        SV *arg = va_arg(ap, SV *) ;
		args[i] = arg ;
	}

	SV *ret = perl_call_helper_function(ctx, rt, func, nb_args, args) ;
	free(args) ;

	return ret ;
}

