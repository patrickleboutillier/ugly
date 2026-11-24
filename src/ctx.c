/* Contexts (one per thread) and structured errors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"


static _Thread_local ugly_ctx *current ;


char *xstrdup(const char *s){
	return strdup(s != NULL ? s : "") ;
}


int on_thread(pthread_t t){
	return pthread_equal(t, pthread_self()) ;
}


/* ---------------------------------------------------------------- */
/* Contexts                                                         */
/* ---------------------------------------------------------------- */

ugly_status ugly_ctx_new(ugly_ctx **out){
	if (out == NULL){
		return UGLY_ERR_USAGE ;
	}
	*out = NULL ;
	if (current != NULL){
		return UGLY_ERR_USAGE ;
	}
	ugly_ctx *ctx = calloc(1, sizeof(ugly_ctx)) ;
	if (ctx == NULL){
		return UGLY_ERR_NOMEM ;
	}
	ctx->thread = pthread_self() ;
	current = ctx ;

	ugly_status st = rt_create(ctx, lang_builtin_c(), NULL, 0, &ctx->host) ;
	if (st != UGLY_OK){
		current = NULL ;
		ugly_error_free(ctx->error) ;
		free(ctx) ;
		return st ;
	}
	*out = ctx ;
	return UGLY_OK ;
}


void ugly_ctx_delete(ugly_ctx *ctx){
	if ((ctx == NULL)||(! on_thread(ctx->thread))||(ctx->top != NULL)){
		return ;
	}
	/* Runtimes ugly_rt_get created, newest first, and the language loads they made. */
	while (ctx->nautos > 0){
		struct auto_rt a = ctx->autos[--ctx->nautos] ;
		if (a.rt != NULL){
			rt_destroy(ctx, a.rt) ;
		}
		if (a.loaded != NULL){
			ugly_lang_unload(ctx, a.loaded) ;
		}
	}
	free(ctx->autos) ;
	rt_destroy(ctx, ctx->host) ;
	ugly_error_free(ctx->error) ;
	if (current == ctx){
		current = NULL ;
	}
	free(ctx) ;
}


ugly_ctx *ugly_current_ctx(void){
	return current ;
}


ugly_status ctx_check(ugly_ctx *ctx){
	if (ctx == NULL){
		return UGLY_ERR_USAGE ;
	}
	if (! on_thread(ctx->thread)){
		/* Not ours to touch: no error is recorded in another thread's ctx. */
		return UGLY_ERR_WRONG_THREAD ;
	}
	if (ctx->error != NULL){
		ugly_error_free(ctx->error) ;
		ctx->error = NULL ;
	}
	return UGLY_OK ;
}


/* ---------------------------------------------------------------- */
/* Errors                                                           */
/* ---------------------------------------------------------------- */

ugly_error *error_new(ugly_status status, const char *language, const char *type, const char *message,
	ugly_handle payload){

	ugly_error *err = calloc(1, sizeof(ugly_error)) ;
	if (err == NULL){
		return NULL ;
	}
	err->status = status ;
	err->language = xstrdup(language) ;
	err->type = xstrdup(type) ;
	err->message = xstrdup(message) ;
	err->payload = payload ;
	return err ;
}


void ctx_set_error(ugly_ctx *ctx, ugly_error *err){
	if (ctx->error != NULL){
		ugly_error_free(ctx->error) ;
	}
	ctx->error = err ;
}


ugly_status ctx_fail(ugly_ctx *ctx, ugly_status status, const char *language, const char *type,
	const char *fmt, ...){

	char msg[512] ;
	va_list ap ;
	va_start(ap, fmt) ;
	vsnprintf(msg, sizeof(msg), fmt, ap) ;
	va_end(ap) ;
	ctx_set_error(ctx, error_new(status, language, type, msg, UGLY_NO_HANDLE)) ;
	return status ;
}


const ugly_error *ugly_last_error(const ugly_ctx *ctx){
	return (ctx != NULL ? ctx->error : NULL) ;
}


ugly_status ugly_error_status(const ugly_error *err){
	return err->status ;
}


const char *ugly_error_language(const ugly_error *err){
	return err->language ;
}


const char *ugly_error_type(const ugly_error *err){
	return err->type ;
}


const char *ugly_error_message(const ugly_error *err){
	return err->message ;
}


/* Doesn't clear the ctx's error: err is usually that error. */
ugly_status ugly_error_payload(ugly_ctx *ctx, const ugly_error *err, ugly_handle *out){
	if ((ctx == NULL)||(err == NULL)||(out == NULL)){
		return UGLY_ERR_USAGE ;
	}
	if (! on_thread(ctx->thread)){
		return UGLY_ERR_WRONG_THREAD ;
	}
	*out = UGLY_NO_HANDLE ;
	if (err->payload == UGLY_NO_HANDLE){
		return UGLY_OK ;
	}
	return h_dup(err->payload, out) ;
}


ugly_status ugly_raise(ugly_ctx *ctx, ugly_status status, const char *type, const char *message,
	ugly_handle payload){

	if ((ctx == NULL)||(! on_thread(ctx->thread))||(status == UGLY_OK)){
		if (payload != UGLY_NO_HANDLE){
			h_release(payload) ;
		}
		return (status == UGLY_OK ? UGLY_ERR_USAGE : status) ;
	}
	ctx_set_error(ctx, error_new(status, "c", type, message, payload)) ;
	return status ;
}


ugly_status ugly_raise_native(ugly_ctx *ctx, ugly_rt *rt, const char *type, const char *message,
	void *payload_ref, uintptr_t payload_identity){

	ugly_handle payload = UGLY_NO_HANDLE ;
	if (payload_ref != NULL){
		if (ugly_export(ctx, rt, payload_ref, payload_identity, &payload) != UGLY_OK){
			payload = UGLY_NO_HANDLE ;
		}
	}
	ctx_set_error(ctx, error_new(UGLY_ERR_RAISED, rt->lang->name, type, message, payload)) ;
	return UGLY_ERR_RAISED ;
}


ugly_status ugly_raise_handle(ugly_ctx *ctx, ugly_rt *rt, const char *type, const char *message, ugly_handle payload){
	ctx_set_error(ctx, error_new(UGLY_ERR_RAISED, rt->lang->name, type, message, payload)) ;
	return UGLY_ERR_RAISED ;
}


ugly_status ugly_fail(ugly_ctx *ctx, ugly_rt *rt, ugly_status status, const char *type, const char *message){
	if (status == UGLY_OK){
		status = UGLY_ERR_INTERNAL ;
	}
	ctx_set_error(ctx, error_new(status, rt->lang->name, type, message, UGLY_NO_HANDLE)) ;
	return status ;
}


ugly_error *ugly_take_error(ugly_ctx *ctx){
	ugly_error *err = ctx->error ;
	ctx->error = NULL ;
	return err ;
}


ugly_status ugly_restore_error(ugly_ctx *ctx, ugly_error *err){
	ctx_set_error(ctx, err) ;
	return err->status ;
}


void ugly_error_free(ugly_error *err){
	if (err == NULL){
		return ;
	}
	if (err->payload != UGLY_NO_HANDLE){
		h_release(err->payload) ;
	}
	free(err->language) ;
	free(err->type) ;
	free(err->message) ;
	free(err) ;
}
