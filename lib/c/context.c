#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "ugly_priv.h"



ugly_context *ugly_context_new(){
	ugly_debug(2, "-> Context new") ;
	ugly_context *ctx = UGLY_MALLOC(ugly_context) ;
	ctx->error = UGLY_OK ;
	ctx->error_msg = NULL ;

	ugly_debug(2, "<- Context new") ;
	return ctx ;
}


void ugly_context_delete(ugly_context *ctx){
	if (ctx != NULL){
		ugly_debug(2, "-> Context delete") ;
		if (ctx->error_msg != NULL){
			free(ctx->error_msg) ;
		}
		free(ctx) ;
		ugly_debug(2, "<- Context delete") ;
	}
}


int ugly_context_has_error(const ugly_context *ctx){
	return (ctx->error == UGLY_OK ? 0 : 1) ;
}


ugly_error ugly_context_get_error(const ugly_context *ctx){
	return ctx->error ;
}


const char *ugly_context_get_error_msg(const ugly_context *ctx){
	return ctx->error_msg ;
}


void ugly_context_set_error(ugly_context *ctx, ugly_error err, const char *fmt, ...){
	ugly_context_clear_error(ctx) ;
	ctx->error = err ;

	if (fmt != NULL){
		va_list va ;
		va_start(va, fmt) ;

		char *msg = (char *)malloc(UGLY_MAX_ERROR_MSG_LENGTH * sizeof(char)) ;
		vsnprintf(msg, UGLY_MAX_ERROR_MSG_LENGTH - 1, fmt, va) ;
		va_end(va) ;
		ctx->error_msg = msg ;
	}
}


void ugly_context_clear_error(ugly_context *ctx){
	ctx->error = UGLY_OK ;
	if (ctx->error_msg != NULL){
		free(ctx->error_msg) ;
		ctx->error_msg = NULL ;
	}
}
