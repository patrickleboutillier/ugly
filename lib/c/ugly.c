#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
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

