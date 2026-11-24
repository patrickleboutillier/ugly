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
 * The directory under which each language plugin finds its helper code, in
 * <dir>/<lang>/. The build sets it to $(pkglibdir); at runtime the
 * UGLY_LANGUAGE_DIR environment variable overrides it (the test suite points
 * it at the source tree's lang/ directory).
*/
#ifndef UGLY_LANGUAGE_DIR
#define UGLY_LANGUAGE_DIR		"/usr/local/lib/ugly"
#endif


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
		ugly_object *o ;
	} v ;
	/* int thrown ; */
	char *stringy ;
} ;


/* UGLY language */
struct _ugly_language {
    char *name ;
    char *dir ;
    void *handle ;
    int nb_runtimes ;		/* live runtimes: the language can't be deleted until 0 */

    ugly_interface *iface ;
} ;


/* UGLY runtime */
struct _ugly_runtime {
    ugly_language *language ;
    void *handle ;
    int nb_objects ;		/* live objects: the runtime can't be deleted until 0 */
} ;


/* UGLY object */
struct _ugly_object {
    ugly_runtime *runtime ;
    char *class ;
    void *handle ;
    char *stringy ;
} ;


#include "ugly_iface.h"


const char *ugly_type_to_string(ugly_type t) ;
ugly_object *ugly_object_alloc(ugly_runtime *rt, const char *class, void *object) ;


#endif

