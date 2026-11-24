#ifndef _UGLY_PERL_PRIV_H_
#define _UGLY_PERL_PRIV_H_

#include "../../lib/c/ugly_priv.h"

#include <EXTERN.h>
#include <perl.h>


/*
 * Every entry into an embedded interpreter must make it the current perl
 * context, and restore the caller's context on the way out. Without this, a
 * perl caller (via the XS binding) or a second perl runtime in the same
 * process continues running against the wrong interpreter after the call.
 */
#define UGLY_PERL5_ENTER(interp) \
	PerlInterpreter *_ugly_perl5_saved = PERL_GET_CONTEXT ; \
	PERL_SET_CONTEXT(interp)
#define UGLY_PERL5_LEAVE() \
	PERL_SET_CONTEXT(_ugly_perl5_saved)


ugly_value *perl_call_helper_function(ugly_context *ctx, ugly_runtime *rt, 
	const char *func, int nb_args, SV **args) ;
ugly_value *perl_call_helper_function_v(ugly_context *ctx, ugly_runtime *rt, 
	const char *func, int nb_args, ...) ;


#endif
