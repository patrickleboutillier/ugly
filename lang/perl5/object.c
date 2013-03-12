#include <stdlib.h>
#include <string.h>
#include "mlp_perl_priv.h"


mlp_value* _mlp_perl_object_new(mlp_context *ctx, mlp_runtime *rt, const char *class, mlp_value **args, int nb_args, const char *hint){
	PerlInterpreter *my_perl = (PerlInterpreter *)rt->handle ;

	dSP ;
	ENTER ;
	SAVETMPS ;

	PUSHMARK(SP) ;
	XPUSHs(sv_2mortal(newSVpv(class, 0))) ;
	int i = 0 ;
	for (i = 0 ; i < nb_args ; i++){
		SV *sv = perl_SV_new(ctx, rt, args[i]) ;
		if (mlp_context_has_error(ctx)){
			return NULL ;
		}
		XPUSHs(sv_2mortal(sv)) ;
	}
	PUTBACK ;

	char *sub = (char *)malloc(strlen(class) + 8) ;
	sprintf(sub, "%s::new", class) ;
    int count = call_pv(sub, G_SCALAR | G_EVAL) ;
	free(sub) ;

	SPAGAIN ;

	if (SvTRUE(ERRSV)){
		/* TODO: Deal with the exception and return it to the caller */
		STRLEN n_a ;
		mlp_context_set_error(ctx, MLP_RUNTIME_ERROR, "Error creating Perl object: %s",
			SvPV(ERRSV, n_a)) ;
		return NULL ;
    }
	else if (count != 1){
		mlp_context_set_error(ctx, MLP_RUNTIME_ERROR, "Perl function must return 1 value (not %d)", count) ;
		return NULL ;
	}

	SV *obj = POPs ;
	SvREFCNT_inc(obj) ;
    mlp_debug(5, "refcnt=%d", SvREFCNT(obj)) ;

    FREETMPS ;
    LEAVE ;

	mlp_object *o = mlp_object_alloc(rt, class, (void *)obj) ;
    mlp_value *val = mlp_value_new_object(o) ;

    return val ;
}


mlp_value* _mlp_perl_object_call_method(mlp_context *ctx, mlp_object *obj, const char *method, mlp_value **args, int nb_args, const char *hint){
	mlp_debug(3, "-> Perl object call method: %s, %s", mlp_object_to_string(obj), method) ;
	mlp_runtime *rt = mlp_object_get_runtime(obj) ;
	PerlInterpreter *my_perl = (PerlInterpreter *)(rt->handle) ;
	mlp_debug(4, "1") ;

	dSP ;
	ENTER ;
	SAVETMPS ;

	PUSHMARK(SP) ;
	mlp_debug(4, "a") ;
	XPUSHs((SV *)mlp_object_get_handle(obj)) ;
	mlp_debug(4, "b") ;
	int i = 0 ;
	for (i = 0 ; i < nb_args ; i++){
		mlp_debug(4, "c:%p", args[i]) ;
		SV *sv = perl_SV_new(ctx, rt, args[i]) ;
		mlp_debug(4, "sv:%p", sv) ;
		if (mlp_context_has_error(ctx)){
			mlp_debug(4, "Yo!") ;
			return NULL ;
		}
		mlp_debug(4, "p1") ;
		XPUSHs(sv_2mortal(sv)) ;
		mlp_debug(4, "p2") ;
	}
	PUTBACK ;

	const char *class = mlp_object_get_class(obj) ;
	char *sub = (char *)malloc(strlen(class) + strlen(method) + 8) ;
	sprintf(sub, "%s::%s", class, method) ;
	mlp_debug(4, "[%s]", sub) ;
    int count = call_method(method, G_SCALAR | G_EVAL) ;
	free(sub) ;

	SPAGAIN ;

	mlp_debug(4, "3") ;
	if (SvTRUE(ERRSV)){
		/* TODO: Deal with the exception and return it to the caller */
		STRLEN n_a ;
		mlp_context_set_error(ctx, MLP_RUNTIME_ERROR, "Error creating Perl object: %s",
			SvPV(ERRSV, n_a)) ;
		return NULL ;
    }
	else if (count != 1){
		mlp_context_set_error(ctx, MLP_RUNTIME_ERROR, "Perl function must return 1 value (not %d)", count) ;
		return NULL ;
	}

    mlp_value *val = NULL ;
	SV *r = POPs ;
	if (SvNOK(r)){
		val = mlp_value_new_double(SvNV(r)) ;
	}
	else if (SvIOK(r)){
		val = mlp_value_new_long(SvIV(r)) ;
	}
	else if (SvPOK(r)){
		STRLEN n_a ;
		val = mlp_value_new_string(SvPV(r, n_a)) ;
	}

    FREETMPS ;
    LEAVE ;


	mlp_debug(3, "<- Perl object call method") ;
    return val ;
}


void _mlp_perl_object_delete(mlp_context *ctx, mlp_object *obj){
	mlp_debug(3, "-> Perl object delete: %s", mlp_object_to_string(obj)) ;
	PerlInterpreter *my_perl = (PerlInterpreter *)(mlp_object_get_runtime(obj)->handle) ;
	mlp_debug(4, "%p", mlp_object_get_handle(obj)) ;

	SvREFCNT_dec((SV *)mlp_object_get_handle(obj)) ;
	mlp_debug(3, "<- Perl object delete") ;
}
