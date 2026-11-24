/* Values: releasing results, and filling results with owned strings and bytes. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"


void ugly_val_release(ugly_ctx *ctx, ugly_val *v){
	if (v == NULL){
		return ;
	}
	switch (v->kind){
		case UGLY_STRING:
			free((void *)v->as.str.ptr) ;
			break ;
		case UGLY_BYTES:
			free((void *)v->as.bytes.ptr) ;
			break ;
		case UGLY_HANDLE:
			ugly_release(ctx, v->as.h) ;
			break ;
		default:
			break ;
	}
	*v = ugly_null() ;
}


static void *copy(const void *p, size_t len){
	char *m = malloc(len + 1) ;    /* +1: strings stay NUL-terminated for convenience */
	if (m == NULL){
		return NULL ;
	}
	if (len > 0){
		memcpy(m, p, len) ;
	}
	m[len] = '\0' ;
	return m ;
}


ugly_status ugly_val_set_string(ugly_ctx *ctx, ugly_val *out, const char *p, size_t len){
	(void)ctx ;
	if ((out == NULL)||((p == NULL)&&(len > 0))){
		return UGLY_ERR_USAGE ;
	}
	char *m = copy(p, len) ;
	if (m == NULL){
		return UGLY_ERR_NOMEM ;
	}
	*out = ugly_str(m, len) ;
	return UGLY_OK ;
}


ugly_status ugly_val_set_bytes(ugly_ctx *ctx, ugly_val *out, const uint8_t *p, size_t len){
	(void)ctx ;
	if ((out == NULL)||((p == NULL)&&(len > 0))){
		return UGLY_ERR_USAGE ;
	}
	uint8_t *m = copy(p, len) ;
	if (m == NULL){
		return UGLY_ERR_NOMEM ;
	}
	*out = ugly_bytes(m, len) ;
	return UGLY_OK ;
}
