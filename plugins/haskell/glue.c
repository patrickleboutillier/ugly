/*
 * C helpers for the Haskell side, compiled by ghc into libugly_hs.so: they
 * read and write ugly_val, so the Haskell code never depends on its layout.
 * The status and kind values Haskell mirrors (Ugly.Native) are pinned here.
 */
#include <stdlib.h>

#include "ugly_plugin.h"

_Static_assert(UGLY_OK == 0 && UGLY_ERR_RAISED == 1 && UGLY_ERR_TYPE == 2 && UGLY_ERR_NOT_FOUND == 3
	&& UGLY_ERR_NOT_CAPABLE == 4 && UGLY_ERR_NOT_SUPPORTED == 9 && UGLY_ERR_NOT_IMPLEMENTED == 10
	&& UGLY_ERR_NOMEM == 12 && UGLY_ERR_INTERNAL == 13, "Ugly.Native's statuses") ;
_Static_assert(UGLY_ANY == 0 && UGLY_NULL == 1 && UGLY_BOOL == 2 && UGLY_INT == 3 && UGLY_DOUBLE == 4
	&& UGLY_STRING == 5 && UGLY_BYTES == 6 && UGLY_HANDLE == 7, "Ugly.Native's kinds") ;
_Static_assert(UGLY_TRAIT_MEMBERS == 1 && UGLY_TRAIT_ELEMENTS == 2 && UGLY_TRAIT_CALLABLE == 4
	&& UGLY_TRAIT_INSTANTIABLE == 8, "Ugly.Native's traits") ;


void ugly_hs_set_null(ugly_val *v){
	*v = ugly_null() ;
}

void ugly_hs_set_bool(ugly_val *v, int b){
	*v = ugly_bool(b != 0) ;
}

void ugly_hs_set_int(ugly_val *v, int64_t i){
	*v = ugly_int(i) ;
}

void ugly_hs_set_double(ugly_val *v, double d){
	*v = ugly_double(d) ;
}

void ugly_hs_set_handle(ugly_val *v, ugly_handle h){
	*v = ugly_hval(h) ;
}


/* member_names: the names, as a core list of strings. */
ugly_status ugly_hs_names(ugly_ctx *ctx, const char *const *names, const size_t *lens, size_t n,
	ugly_handle *out){
	ugly_val *items = malloc((n > 0 ? n : 1) * sizeof *items) ;
	if (items == NULL){
		return UGLY_ERR_NOMEM ;
	}
	for (size_t i = 0 ; i < n ; i++){
		items[i] = ugly_str(names[i], lens[i]) ;
	}
	ugly_status st = ugly_list_new(ctx, items, n, out) ;
	free(items) ;
	return st ;
}
