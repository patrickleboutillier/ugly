#ifndef _UGLY_PRIV_H_
#define _UGLY_PRIV_H_


#include "../../config.h"
#include "ugly.h"


#define UGLY_MAX_DEBUG_MSG_LENGTH	1024
#define UGLY_MAX_ERROR_MSG_LENGTH	1024

#define UGLY_MALLOC(t) 				((t *)malloc(sizeof(t)))
#define UGLY_MALLOC_N(t, n)			((t *)malloc((n) * sizeof(t)))
#define UGLY_CALLOC(t) 				((t *)calloc(sizeof(t), 1))
#define UGLY_CALLOC_N(t, n)			((t *)calloc(sizeof(t), (n)))

#define RETURN_NULL_IF_NULL(c)		if (c == NULL){return NULL ;}


/* 
 * The head directory where langauge plugins can look for there helper code
 * in there own language plugin language.
*/ 
#define UGLY_LANGUAGE_DIR		"/usr/lib/ugly"


struct _ugly_interface ;
typedef struct _ugly_interface ugly_interface ;


const ugly_interface *ugly_language_get_iface(const ugly_language *lang) ;


/* UGLY context */
struct _ugly_context {
    ugly_error error ;
    char *error_msg ;
} ;


/* UGLY value: A value can be passed as a function argument or returned by a function */
struct _ugly_value {
	ugly_type type ;
	union {
		char c ;
		long l ;
		double d ;
		char *s ;
	} v ;
	/* int thrown ; */
	char *stringy ;
} ;


/* UGLY language */
struct _ugly_language {
    char *name ;
    char *dir ;
    void *handle ;

    ugly_interface *iface ;
} ;


/* UGLY runtime */
struct _ugly_runtime {
    ugly_language *language ;
    void *handle ;
} ;


#include "ugly_iface.h"


#endif

