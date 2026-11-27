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

/* ugly_ctx_thread's ctxs, deleted by this key's destructor when their thread exits. */
static pthread_key_t thread_key ;
static pthread_once_t thread_key_once = PTHREAD_ONCE_INIT ;

static void ctx_at_thread_exit(void *p){
	if (p == current){
		ugly_ctx_delete(p) ;
	}
}

static void make_thread_key(void){
	pthread_key_create(&thread_key, ctx_at_thread_exit) ;
}


ugly_status ugly_ctx_thread(ugly_ctx **out){
	if (out == NULL){
		return UGLY_ERR_USAGE ;
	}
	if (current != NULL){
		*out = current ;
		return UGLY_OK ;
	}
	ugly_status st = ugly_ctx_new(out) ;
	if (st == UGLY_OK){
		pthread_once(&thread_key_once, make_thread_key) ;
		pthread_setspecific(thread_key, *out) ;
	}
	return st ;
}


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
	lang_fork_safety() ;

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
	/*
	 * Runtimes ugly_rt_get gave this ctx, newest first, and the language
	 * loads they made. Each is a share: the last one destroys a runtime
	 * ugly_rt_get created (on whichever thread that is).
	 */
	while (ctx->nautos > 0){
		struct auto_rt a = ctx->autos[--ctx->nautos] ;
		if (a.rt != NULL){
			rt_unshare(ctx, a.rt) ;
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

/*
 * An error's text, as valid UTF-8 whatever it was given. Plugins build
 * messages in fixed buffers, which cut a long one in the middle of a
 * character, and some languages' strings are bytes (Lua's, Perl's): a
 * language whose strings are strict (Python's, C#'s) would then fail to
 * build its exception, and lose the error it was reporting. A sequence cut
 * short by the end of the string is dropped; any other byte that isn't
 * UTF-8 becomes U+FFFD.
 */
static size_t utf8_len(const unsigned char *s, size_t left){
	unsigned char c = s[0] ;
	size_t n = (c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0), i ;
	if ((n == 0)||(n > left)){
		return 0 ;
	}
	for (i = 1 ; i < n ; i++){
		if ((s[i] & 0xC0) != 0x80){
			return 0 ;
		}
	}
	uint32_t cp = (n == 1 ? c : n == 2 ? (uint32_t)(c & 0x1F) : n == 3 ? (uint32_t)(c & 0x0F) : (uint32_t)(c & 0x07)) ;
	for (i = 1 ; i < n ; i++){
		cp = (cp << 6) | (s[i] & 0x3F) ;
	}
	/* No overlong forms, no surrogates, nothing past U+10FFFF. */
	if (((n == 2)&&(cp < 0x80))||((n == 3)&&(cp < 0x800))||((n == 4)&&(cp < 0x10000))
		||((cp >= 0xD800)&&(cp <= 0xDFFF))||(cp > 0x10FFFF)){
		return 0 ;
	}
	return n ;
}


/* Whether s[0..left) is the start of a sequence that the end of the string cut. */
static int utf8_cut(const unsigned char *s, size_t left){
	unsigned char c = s[0] ;
	size_t n = ((c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0), i ;
	if ((n == 0)||(left >= n)){
		return 0 ;
	}
	for (i = 1 ; i < left ; i++){
		if ((s[i] & 0xC0) != 0x80){
			return 0 ;
		}
	}
	return 1 ;
}


static char *text_dup(const char *text){
	const unsigned char *s = (const unsigned char *)(text != NULL ? text : "") ;
	size_t len = strlen((const char *)s), i = 0, o = 0 ;
	char *out = malloc(3 * len + 1) ;
	if (out == NULL){
		return NULL ;
	}
	while (i < len){
		size_t n = utf8_len(s + i, len - i) ;
		if (n > 0){
			memcpy(out + o, s + i, n) ;
			o += n ;
			i += n ;
		}
		else if (utf8_cut(s + i, len - i)){
			break ;
		}
		else {
			memcpy(out + o, "\xEF\xBF\xBD", 3) ;
			o += 3 ;
			i++ ;
		}
	}
	out[o] = '\0' ;
	return out ;
}


ugly_error *error_new(ugly_status status, const char *language, const char *type, const char *message,
	ugly_handle payload){

	ugly_error *err = calloc(1, sizeof(ugly_error)) ;
	if (err == NULL){
		return NULL ;
	}
	err->status = status ;
	err->language = text_dup(language) ;
	err->type = text_dup(type) ;
	err->message = text_dup(message) ;
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
