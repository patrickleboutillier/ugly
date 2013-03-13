#include <stdlib.h>
#include <string.h>
#include "ugly_perl5_priv.h"


static void xs_init (pTHX) ;
EXTERN_C void boot_DynaLoader (pTHX_ CV* cv) ;
EXTERN_C void xs_init(pTHX){
    char *file = __FILE__ ;
    /* DynaLoader is a special case */
    newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file) ;
}


void *_ugly_perl5_runtime_new(ugly_context *ctx, ugly_language *lang){
	PerlInterpreter *my_perl = NULL ;
	char *embedding[] = { "", "-e", "0" } ;

	my_perl = perl_alloc() ;
	perl_construct(my_perl) ;
	perl_parse(my_perl, xs_init, 3, embedding, NULL) ;
	PL_exit_flags |= PERL_EXIT_DESTRUCT_END ;
	perl_run(my_perl) ;

	const char *dir = ugly_language_get_dir(lang) ;
	char *buf = (char *)malloc((strlen(dir) + 64) * sizeof(char)) ;
	sprintf(buf, "require '%s/helper.pm' ;", dir) ;
	ugly_debug(3, "Loading helper perl5 module %s/helper.pm", dir) ;
	eval_pv(buf, FALSE) ;
	if (SvTRUE(ERRSV)){
		STRLEN n_a ;
		ugly_context_set_error(ctx, UGLY_LIBRARY_ERROR, "Can't load perl5 helper module %s/helper.pm: %s",
			dir, SvPV(ERRSV, n_a)) ;
		return NULL ;
	}
	ugly_debug(3, "Helper perl5 module %s/helper.pm loaded", dir) ;

	return my_perl ;
}


void _ugly_perl5_runtime_load_library(ugly_context *ctx, ugly_runtime *rt, const char *lib){
	PerlInterpreter *my_perl = (PerlInterpreter *)rt->handle ;
	perl_call_helper_function_v(ctx, rt, "Ugly::perl5::load_library", 1, newSVpv(lib, 0)) ;
}


ugly_value *_ugly_perl5_runtime_call_function(ugly_context *ctx, ugly_runtime *rt, ugly_type rtype, 
	const char *func, ugly_value **args, int nb_args, const char *hint){

	PerlInterpreter *my_perl = (PerlInterpreter *)rt->handle ;

	SV **svs = UGLY_MALLOC_N(SV *, nb_args + 2) ;
	svs[0] = newSViv(rtype) ;
	svs[1] = newSVpv(func, 0) ;
	int i = 0 ;
	for (i = 0; i < nb_args ; i++){
		svs[i + 2] = sv_setref_pv(newSVuv(0), "Ugly::Value", (void *)args[i]) ;
	}

	ugly_debug(3, "Calling perl5 function Ugly::perl5::call_function") ;
	ugly_value *ret = perl_call_helper_function(ctx, rt, "Ugly::perl5::call_function", nb_args + 2, svs) ;
	ugly_debug(3, "perl5 function Ugly::perl5::call_function called") ;

	free(svs) ;

	return ret ;
}


