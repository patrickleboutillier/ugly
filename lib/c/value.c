#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>


#include "ugly_priv.h"


static ugly_value *ugly_value_new(ugly_type t){
    ugly_value *v = UGLY_MALLOC(ugly_value) ;
    v->type = t ;
    v->stringy = NULL ;

    return v ;
}


ugly_value *ugly_value_new_bool(char val){
	ugly_value *v = ugly_value_new(UGLY_BOOL) ;
	v->v.c = (val ? 1 : 0) ;

	return v ;
}


ugly_value *ugly_value_new_long(long val){
	ugly_value *v = ugly_value_new(UGLY_LONG) ;
	v->v.l = val ;

	return v ;
}


ugly_value *ugly_value_new_double(double val){
	ugly_value *v = ugly_value_new(UGLY_DOUBLE) ;
	v->v.d = val ;

	return v ;
}


ugly_value *ugly_value_new_char(char val){
	ugly_value *v = ugly_value_new(UGLY_CHAR) ;
	v->v.c = val ;

	return v ;
}


ugly_value *ugly_value_new_string(const char *val){
	ugly_value *v = ugly_value_new(UGLY_STRING) ;
	v->v.s = (val == NULL ? NULL : strdup(val)) ;

	return v ;
}


ugly_value *ugly_value_new_object(ugly_object *val){
	ugly_value *v = ugly_value_new(UGLY_OBJECT) ;
	v->v.o = val ;

	return v ;
}


void ugly_value_delete(ugly_value *val){
	ugly_debug(2, "-> Value delete: %s", ugly_value_to_string(val)) ;
	switch (val->type){
		case UGLY_STRING:
			if (val->v.s != NULL){
				free(val->v.s) ;
			}
			break ;
    }
	if (val->stringy != NULL){
		free(val->stringy) ;
	}

	free(val) ;
	ugly_debug(2, "<- Value delete") ;
}


ugly_type ugly_value_get_type(const ugly_value *val){
	return val->type ;
}


char ugly_value_get_bool(const ugly_value *val){
	assert(val->type == UGLY_BOOL) ;
	return (val->v.c ? 1 : 0) ;
}


long ugly_value_get_long(const ugly_value *val){
	assert(val->type == UGLY_LONG) ;
	return val->v.l ;
}


double ugly_value_get_double(const ugly_value *val){
	assert(val->type == UGLY_DOUBLE) ;
	return val->v.d ;
}


char ugly_value_get_char(const ugly_value *val){
	assert(val->type == UGLY_CHAR) ;
	return val->v.c ;
}


const char *ugly_value_get_string(const ugly_value *val){
	assert(val->type == UGLY_STRING) ;
	return val->v.s ;
}


ugly_object *ugly_value_get_object(const ugly_value *val){
    assert(val->type == UGLY_OBJECT) ;
    return val->v.o ;
}


const char *ugly_value_to_string(ugly_value *val){
	if (val->stringy != NULL){
		return val->stringy ;
	}

	switch (val->type){
		case UGLY_BOOL: {
			char b[32] ;
			snprintf(b, 31, "BOOL(%s)", val->v.l ? "true" : "false") ;
			val->stringy = strdup(b) ;
		}
		case UGLY_LONG: {
			char num[1024] ;
			snprintf(num, 1023, "LONG(%ld)", val->v.l) ;
			val->stringy = strdup(num) ;
			return val->stringy ;
		}
		case UGLY_DOUBLE: {
			char num[1024] ;
			snprintf(num, 1023, "DOUBLE(%.15lf)", val->v.d) ;
			val->stringy = strdup(num) ;
			return val->stringy ;
		}
		case UGLY_CHAR: {
			char c[8] ;
			snprintf(c, 1, "CHAR(%c)", val->v.c) ;
			val->stringy = strdup(c) ;
			return val->stringy ;
		}
		case UGLY_STRING: {
			if (val->v.s != NULL){
				int len = strlen(val->v.s) + 32 ;
				val->stringy = (char *)malloc(len * sizeof(char)) ;
				snprintf(val->stringy, len-1, "STRING(%s)", val->v.s) ;
			}
			else {
				val->stringy = strdup("STRING(NULL)") ; 
			}
			return val->stringy ;
		}
        case UGLY_OBJECT: {
            if (val->v.o != NULL){
                const char *ostr = ugly_object_to_string(val->v.o) ;
                int len = strlen(ostr) + 32 ;
                val->stringy = (char *)malloc(len * sizeof(char)) ;
                snprintf(val->stringy, len-1, "OBJECT(%s)", ostr) ;
            }
            else {
                val->stringy = strdup("OBJECT(NULL)") ;
            }
            return val->stringy ;
        }
    }

	return val->stringy ;
}
