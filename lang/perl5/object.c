#include <stdlib.h>
#include <string.h>
#include "ugly_perl5_priv.h"


ugly_object* _ugly_perl5_object_new(ugly_context *ctx, ugly_runtime *rt, const char *class, 
	ugly_value **args, int nb_args, const char *hint){

	PerlInterpreter *my_perl = (PerlInterpreter *)rt->handle ;

	int extra = 1 ;
	SV **svs = UGLY_MALLOC_N(SV *, nb_args + extra) ;
	svs[0] = newSVpv(class, 0) ;
	int i = 0 ;
	for (i = 0; i < nb_args ; i++){
		svs[i + extra] = sv_setref_pv(newSVuv(0), "Ugly::Value", (void *)args[i]) ;
	}

	ugly_debug(3, "Calling perl5 function Ugly::perl5::new_object") ;
	ugly_value *ret = perl_call_helper_function(ctx, rt, "Ugly::perl5::new_object", nb_args + extra, svs) ;
	ugly_debug(3, "perl5 function Ugly::perl5::new_object called: %s", ugly_value_to_string(ret)) ;

	free(svs) ;
	if (ret == NULL){
		return NULL ;
	}

	ugly_object *obj = ugly_value_get_object(ret) ;
	ugly_value_delete(ret) ;
	return obj ;
}


ugly_value* _ugly_perl5_object_call_method(ugly_context *ctx, ugly_object *obj, ugly_type rtype, 
	const char *method, ugly_value **args, int nb_args, const char *hint){

	ugly_runtime *rt = ugly_object_get_runtime(obj) ;
	PerlInterpreter *my_perl = (PerlInterpreter *)rt->handle ;

	int extra = 3 ;
	SV **svs = UGLY_MALLOC_N(SV *, nb_args + extra) ;
	svs[0] = ugly_object_get_handle(obj) ;
	svs[1] = newSViv(rtype) ;
	svs[2] = newSVpv(method, 0) ;
	int i = 0 ;
	for (i = 0; i < nb_args ; i++){
		svs[i + extra] = sv_setref_pv(newSVuv(0), "Ugly::Value", (void *)args[i]) ;
	}

	ugly_debug(3, "Calling perl5 function Ugly::perl5::call_method: %s", ugly_object_to_string(obj)) ;
	ugly_value *ret = perl_call_helper_function(ctx, rt, "Ugly::perl5::call_method", nb_args + extra, svs) ;
	ugly_debug(3, "perl5 function Ugly::perl5::call_method called") ;

	free(svs) ;

	return ret ;
}


void _ugly_perl5_object_delete(ugly_context *ctx, ugly_object *obj){
	ugly_debug(3, "-> Perl object delete: %s", ugly_object_to_string(obj)) ;
	PerlInterpreter *my_perl = (PerlInterpreter *)(ugly_object_get_runtime(obj)->handle) ;
	ugly_debug(4, "%p", ugly_object_get_handle(obj)) ;

	SvREFCNT_dec((SV *)ugly_object_get_handle(obj)) ;
	ugly_debug(3, "<- Perl object delete") ;
}

