#include <stdlib.h>
#include "ugly_perl5_priv.h"


extern char **environ ;


/*
 * perlembed requires PERL_SYS_INIT3 once per process before the first
 * interpreter. We call it only if we are the ones starting perl: once any
 * interpreter has existed in the process (ours, or the host's when the host is
 * itself perl, e.g. through the Perl binding), PL_curinterp is set, and it
 * stays set after perl_free.
 *
 * PERL_SYS_TERM is deliberately never called. It is a process-exit operation:
 * it tears down PerlIO's global state, and perl can't start again in the same
 * process afterwards (perl_alloc then dies with EBADF). Calling it when the
 * last language is deleted would make a later ugly_language_init fail.
*/
ugly_interface *_ugly_perl5_language_init(ugly_context *ctx, const char *name, const char *dir){
	if (PL_curinterp == NULL){
		static char *args[] = { "", NULL } ;
		int argc = 1 ;
		char **argv = args ;
		char **env = environ ;
		ugly_debug(3, "PERL_SYS_INIT3") ;
		PERL_SYS_INIT3(&argc, &argv, &env) ;
	}

	ugly_interface *iface = UGLY_INIT_IFACE(iface, perl5) ;

    return iface ;
}


/* Nothing to do: process-wide perl state outlives the language (see above). */
void _ugly_perl5_language_delete(ugly_context *ctx, ugly_language *lang){
}
