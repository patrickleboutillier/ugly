#ifndef _UGLY_PERL_PRIV_H_
#define _UGLY_PERL_PRIV_H_

#include "../../lib/c/ugly_priv.h"

#include <EXTERN.h>
#include <perl.h>


ugly_value *perl_call_helper_function(ugly_context *ctx, ugly_runtime *rt, 
	const char *func, int nb_args, SV **args) ;
ugly_value *perl_call_helper_function_v(ugly_context *ctx, ugly_runtime *rt, 
	const char *func, int nb_args, ...) ;


#endif
