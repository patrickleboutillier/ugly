/* Loading a language plugin (libugly_<lang>.so) and its interface. */
#include "ugly_test.h"


int main(void){
	const char *name = ugly_test_lang() ;
	ugly_context *ctx = ugly_context_new() ;

	ugly_language *lang = ugly_language_init(ctx, name) ;
	if (no_error(ctx, "language %s initialized", name)){
		ok(lang != NULL, "language handle returned") ;
		is_string(ugly_language_get_name(lang), name, "language name") ;

		const char *dir = ugly_language_get_dir(lang) ;
		ok(dir != NULL, "language directory is %s", dir ? dir : "NULL") ;
		size_t dl = (dir ? strlen(dir) : 0), nl = strlen(name) ;
		ok((dl > nl)&&(dir[dl - nl - 1] == '/')&&(strcmp(dir + dl - nl, name) == 0),
			"language directory ends with /%s", name) ;

		ugly_language_delete(ctx, lang) ;
		no_error(ctx, "language deleted") ;

		/* A language can be loaded again after it was deleted. */
		lang = ugly_language_init(ctx, name) ;
		if (no_error(ctx, "language %s initialized again after delete", name)){
			is_string(ugly_language_get_name(lang), name, "re-initialized language name") ;
			ugly_language_delete(ctx, lang) ;
			no_error(ctx, "re-initialized language deleted") ;
		}
	}

	ugly_language_delete(ctx, NULL) ;
	no_error(ctx, "deleting a NULL language is a no-op") ;

	ugly_language *bad = ugly_language_init(ctx, "no_such_language") ;
	ok(bad == NULL, "unknown language: no handle returned") ;
	has_error(ctx, UGLY_INTERNAL_ERROR, "unknown language: UGLY_INTERNAL_ERROR") ;
	ok(ugly_context_get_error_msg(ctx) != NULL
		&& strstr(ugly_context_get_error_msg(ctx), "libugly_no_such_language.so") != NULL,
		"unknown language: message names the missing plugin") ;

	ugly_context_delete(ctx) ;
	return done_testing() ;
}
