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


const char *ugly_debug_args(ugly_value **args, int nb_args){
		static char s[1024] ;

		s[0] = '\0' ;
	    int i = 0 ;
	    for (i = 0; i < nb_args ; i++){	
			if (i > 0){
				strcat(s, ",") ;
			}

			const char *n = ugly_value_to_string(args[i]) ;
			if ((strlen(n) + strlen(s)) <= 1023){
				strcat(s, ugly_value_to_string(args[i])) ;
			}
	    }

		return s ;
}


const char *ugly_type_to_string(ugly_type t){
	static char s[32] ;

    switch (t){
        case UGLY_BOOL:
			strcpy(s, "BOOL") ; break ;
        case UGLY_LONG:
			strcpy(s, "LONG") ; break ;
        case UGLY_DOUBLE:
			strcpy(s, "DOUBLE") ;break ;
        case UGLY_CHAR:
			strcpy(s, "CHAR") ; break ;
        case UGLY_STRING:
			strcpy(s, "STRING") ; break ;
        case UGLY_OBJECT:
			strcpy(s, "OBJECT") ; break ;
		default:
			strcpy(s, "???") ;
	}

	return s ;
}
