#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#include "ppport.h"

#include "ugly_priv.h"


MODULE = Ugly::Context		PACKAGE = Ugly::Context
PROTOTYPES: ENABLE


ugly_context *
new(pkg)
	const char *pkg
	PREINIT:
		pkg = NULL ;
	CODE:
		RETVAL = ugly_context_new() ;
	OUTPUT:
		RETVAL


void 
delete(this)
	ugly_context *this
	PREINIT:
		this = NULL ;
	CODE:
		ugly_context_delete(this) ;


int
has_error(this)
	ugly_context *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_context_has_error(this) ;
	OUTPUT:
		RETVAL


ugly_error 
get_error(this)
	ugly_context *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_context_get_error(this) ;
	OUTPUT:
		RETVAL


const char *
get_error_msg(this)
	ugly_context *this 
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_context_get_error_msg(this) ;
	OUTPUT:
		RETVAL


void
set_error(this, err, msg)
	ugly_context *this
	ugly_error err
	SV *msg
	PREINIT:
		this = NULL ;
		char *msg_str = NULL ;
	CODE:
		if (SvOK(msg)){
			msg_str = SvPV_nolen(msg) ;
			ugly_context_set_error(this, err, "%s", msg_str) ;
		}
		else {
			ugly_context_set_error(this, err, NULL) ;
		}


void
clear_error(this)
	ugly_context *this
	PREINIT:
		this = NULL ;
	CODE:
		ugly_context_clear_error(this) ;



MODULE = Ugly::Value		PACKAGE = Ugly::Value
PROTOTYPES: ENABLE


ugly_value *
new_bool(pkg, val)
	const char *pkg
	char val
	PREINIT:
		pkg = NULL ;
	CODE:
		RETVAL = ugly_value_new_bool(val) ;
	OUTPUT:
		RETVAL


ugly_value *
new_long(pkg, val)
	const char *pkg
	long val
	PREINIT:
		pkg = NULL ;
	CODE:
		RETVAL = ugly_value_new_long(val) ;
	OUTPUT:
		RETVAL


ugly_value *
new_double(pkg, val)
	const char *pkg
	double val
	PREINIT:
		pkg = NULL ;
	CODE:
		RETVAL = ugly_value_new_double(val) ;
	OUTPUT:
		RETVAL


ugly_value *
new_char(pkg, val)
	const char *pkg
	char val
	PREINIT:
		pkg = NULL ;
	CODE:
		RETVAL = ugly_value_new_char(val) ;
	OUTPUT:
		RETVAL


ugly_value *
new_string(pkg, val)
	const char *pkg
	SV *val
	PREINIT:
		pkg = NULL ;
		char *val_str = NULL ;
	CODE:
		if (SvOK(val)){
			val_str = SvPV_nolen(val) ;
		}
		RETVAL = ugly_value_new_string(val_str) ;
	OUTPUT:
		RETVAL


ugly_value *
new_object(pkg, val)
	const char *pkg
	ugly_object *val
	PREINIT:
		pkg = NULL ;
		val = NULL ;
	CODE:
		RETVAL = ugly_value_new_object(val) ;
	OUTPUT:
		RETVAL


void 
delete(this)
	ugly_value *this
	PREINIT:
		this = NULL ;
	CODE:
		ugly_value_delete(this) ;


ugly_type 
get_type(this)
	ugly_value *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_value_get_type(this) ;
	OUTPUT:
		RETVAL


char 
get_bool(this)
	ugly_value *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_value_get_bool(this) ;
	OUTPUT:
		RETVAL


long 
get_long(this)
	ugly_value *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_value_get_long(this) ;
	OUTPUT:
		RETVAL


double 
get_double(this)
	ugly_value *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_value_get_double(this) ;
	OUTPUT:
		RETVAL


char 
get_char(this)
	ugly_value *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_value_get_char(this) ;
	OUTPUT:
		RETVAL


const char * 
get_string(this)
	ugly_value *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_value_get_string(this) ;
	OUTPUT:
		RETVAL


ugly_object * 
get_object(this)
	ugly_value *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_value_get_object(this) ;
	OUTPUT:
		RETVAL


const char * 
to_string(this)
	ugly_value *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_value_to_string(this) ;
	OUTPUT:
		RETVAL



MODULE = Ugly::Language		PACKAGE = Ugly::Language
PROTOTYPES: ENABLE


ugly_language *
init(pkg, ctx, name)
	const char *pkg
	ugly_context *ctx
	const char *name
	PREINIT:
		pkg = NULL ;
		ctx = NULL ;
	CODE:
		RETVAL = ugly_language_init(ctx, name) ;
	OUTPUT:
		RETVAL


const char *
get_name(this)
	ugly_language *this 
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_language_get_name(this) ;
	OUTPUT:
		RETVAL


const char *
get_dir(this)
	ugly_language *this 
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_language_get_dir(this) ;
	OUTPUT:
		RETVAL


	
MODULE = Ugly::Runtime		PACKAGE = Ugly::Runtime
PROTOTYPES: ENABLE


ugly_runtime *
new(pkg, ctx, lang)
	const char *pkg
	ugly_context *ctx
	ugly_language *lang
	PREINIT:
		pkg = NULL ;
		ctx = NULL ;
		lang = NULL ;
	CODE:
		RETVAL = ugly_runtime_new(ctx, lang) ;
		if (RETVAL == NULL){
			RETVAL = NULL ;
		}
	OUTPUT:
		RETVAL


# ugly_language*				ugly_runtime_get_language	(const ugly_runtime *rt) ;
# UGLY_IFACE void 			ugly_runtime_load_library	(ugly_context *ctx, ugly_runtime *rt, const char *lib) ;
# UGLY_IFACE ugly_value*		ugly_runtime_call_function	(ugly_context *ctx, ugly_runtime *rt, const char *func, ugly_value **args, int nb_args, const char *hint) ;



MODULE = Ugly		PACKAGE = Ugly::Object
PROTOTYPES: ENABLE

ugly_runtime * 
get_runtime(this)
	ugly_object *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_object_get_runtime(this) ;
	OUTPUT:
		RETVAL


const char * 
get_class(this)
	ugly_object *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_object_get_class(this) ;
	OUTPUT:
		RETVAL


void *
get_handle(this)
	ugly_object *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_object_get_handle(this) ;
	OUTPUT:
		RETVAL


const char * 
to_string(this)
	ugly_object *this
	PREINIT:
		this = NULL ;
	CODE:
		RETVAL = ugly_object_to_string(this) ;
	OUTPUT:
		RETVAL


ugly_object * 
wrap(pkg, ctx, rt, ref)
	const char *pkg
	ugly_context *ctx
	ugly_runtime *rt
	SV *ref
	PREINIT:
		const char *class ;
		SV *copy = NULL ;
		ctx = NULL ;
		rt = NULL ;
	CODE:
		/* TODO: Make sure ref is a reference blessed SV sv_isobject , SvSTASH, HvNAME*/
		if (sv_isobject(ref)){
			class = HvNAME(SvSTASH(SvRV(ref))) ;
			copy = newSVsv(ref) ;
			SvREFCNT_inc(copy) ;
			RETVAL = ugly_object_alloc(rt, class, copy) ;
		}
		else {
			croak("ref is not an object reference") ;
		}
	OUTPUT:
		RETVAL


# UGLY_IFACE ugly_value*      ugly_object_new             (ugly_context *ctx, ugly_runtime* rt, const char *class, ugly_value **args, int nb_args, const char *hint) ;
# UGLY_IFACE ugly_value*      ugly_object_call_method     (ugly_context *ctx, ugly_object *obj, ugly_type rtype, const char *method, ugly_value **args, int nb_args, const char *hint) ;
# UGLY_IFACE void             ugly_object_delete          (ugly_context *ctx, ugly_object *obj) ;



MODULE = Ugly		PACKAGE = Ugly
PROTOTYPES: ENABLE



