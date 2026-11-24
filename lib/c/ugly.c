#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "ugly_priv.h"


int ugly_debug_level(){
	const char *do_debug = getenv("UGLY_DEBUG") ;
	if ((do_debug != NULL)&&(do_debug[0] != '\0')){
		int lvl = atoi(do_debug) ;
		return lvl ;
	}

	return 0 ;
}


int ugly_debug_on(int level){
	return (level <= ugly_debug_level()) ;
}


void _ugly_debug(const char *name, int level, const char *msg){
	FILE *fd = stderr ;
	const char *env = getenv("UGLY_TEST_UNDER_HARNESS") ;
	if ((env != NULL)&&(strcmp(env, "") != 0)&&(strcmp(env, "0") != 0)){
		fd = stdout ;
	}

	if (ugly_debug_on(level)){
		fprintf(fd, "# DEBUG(%-8.8s): %*s%s\n", name, level, "", msg) ;
		fflush(fd) ;
	}
}


void ugly_debug(int level, const char *fmt, ...){
        va_list va ;
        va_start(va, fmt) ;
        char *msg = (char *)malloc(UGLY_MAX_DEBUG_MSG_LENGTH * sizeof(char)) ;
        vsnprintf(msg, UGLY_MAX_DEBUG_MSG_LENGTH - 1, fmt, va) ;
        va_end(va) ;

		_ugly_debug("c", level, msg) ;
		free(msg) ;
}


/*
 * Formats args for debug messages. The result lives in a per-thread buffer and
 * is valid until the next call in the same thread. Output that doesn't fit is
 * cut and ends with "...".
*/
const char *ugly_debug_args(ugly_value **args, int nb_args){
	static _Thread_local char s[UGLY_MAX_DEBUG_MSG_LENGTH] ;
	const size_t max = sizeof(s) ;
	const char *ellipsis = "..." ;
	size_t len = 0 ;

	s[0] = '\0' ;
	int i = 0 ;
	for (i = 0 ; i < nb_args ; i++){
		const char *n = ugly_value_to_string(args[i]) ;
		size_t need = strlen(n) + (i > 0 ? 1 : 0) ;
		if (len + need >= max){
			/* Doesn't fit: end with the ellipsis, overwriting the tail if necessary. */
			size_t at = (len + strlen(ellipsis) < max ? len : max - strlen(ellipsis) - 1) ;
			strcpy(s + at, ellipsis) ;
			break ;
		}
		len += snprintf(s + len, max - len, "%s%s", (i > 0 ? "," : ""), n) ;
	}

	return s ;
}


const char *ugly_type_to_string(ugly_type t){
	switch (t){
		case UGLY_BOOL:		return "BOOL" ;
		case UGLY_LONG:		return "LONG" ;
		case UGLY_DOUBLE:	return "DOUBLE" ;
		case UGLY_CHAR:		return "CHAR" ;
		case UGLY_STRING:	return "STRING" ;
		case UGLY_OBJECT:	return "OBJECT" ;
	}

	return "???" ;
}
