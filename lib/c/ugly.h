#ifndef _UGLY_H_
#define _UGLY_H_


#define UGLY_VERSION "0.01"


/* Error codes */
enum _ugly_error {
	UGLY_NOT_SUPPORTED = -1000,  /* Language does not support this feature */
	UGLY_NOT_IMPLEMENTED = -1001, /* Language supports this feature but plugin doesn't implement it yet... */

	UGLY_OK = 0,
	UGLY_INTERNAL_ERROR,
	UGLY_RUNTIME_ERROR,
	UGLY_LIBRARY_ERROR,
} ;
typedef enum _ugly_error ugly_error ;


/* Data types */
enum _ugly_type {
	UGLY_BOOL,
	UGLY_LONG,
	UGLY_DOUBLE,
	UGLY_CHAR,
	UGLY_STRING
} ;
typedef enum _ugly_type ugly_type ;


/* Other types, all opaque... */
struct _ugly_context ;
typedef struct _ugly_context ugly_context ;
struct _ugly_value ;
typedef struct _ugly_value ugly_value ;

struct _ugly_language ;
typedef struct _ugly_language ugly_language ;
struct _ugly_runtime ;
typedef struct _ugly_runtime ugly_runtime ;
struct _ugly_library ;
typedef struct _ugly_library ugly_library ;


/* Public API */
#include "ugly_api.h"


#endif

