/* 
	This header file is parsed by scripts to generate code. The following simple standards must be followed:
	- One token for the return type (ugly_context*, not ugly_context *)
	- One function definition per line, one line per function definition...
*/


#define UGLY_IFACE
#define UGLY_IFACE_V
#define const_char 			const char


/* ugly api */
void						ugly_debug					(int level, const char *fmt, ...) ;
const_char*					ugly_debug_args				(ugly_value **args, int nb_args) ;
int							ugly_debug_on				(int level) ;
int							ugly_debug_level			() ;


/* ugly_context api */
ugly_context*				ugly_context_new 			() ;
int 						ugly_context_has_error 		(const ugly_context *ctx) ;
ugly_error 					ugly_context_get_error 		(const ugly_context *ctx) ;
const_char*					ugly_context_get_error_msg	(const ugly_context *ctx) ;
void 						ugly_context_set_error 		(ugly_context *ctx, ugly_error error, const char *fmt, ...) ; 
void 						ugly_context_clear_error 	(ugly_context *ctx) ;
void 						ugly_context_delete			(ugly_context *ctx) ;


/* ugly_value api */
ugly_value*					ugly_value_new_bool			(char val) ;
ugly_value*					ugly_value_new_long			(long val) ;
ugly_value*					ugly_value_new_double		(double val) ;
ugly_value*					ugly_value_new_char			(char val) ;
ugly_value*					ugly_value_new_string		(const char *val) ;
ugly_value*					ugly_value_new_object		(ugly_object *val) ;
void 						ugly_value_delete			(ugly_value *val) ;
ugly_type 					ugly_value_get_type			(const ugly_value *val) ;
char 						ugly_value_get_bool			(const ugly_value *val) ;
long 						ugly_value_get_long			(const ugly_value *val) ;
double 						ugly_value_get_double		(const ugly_value *val) ;
char 						ugly_value_get_char			(const ugly_value *val) ;
const_char*					ugly_value_get_string		(const ugly_value *val) ;
ugly_object*				ugly_value_get_object		(const ugly_value *val) ;
const_char*					ugly_value_to_string		(ugly_value *val) ;


/* ugly_language api */
ugly_language* 				ugly_language_init			(ugly_context *ctx, const char *name) ;
const_char* 				ugly_language_get_name		(const ugly_language *lang) ;
const_char* 				ugly_language_get_dir		(const ugly_language *lang) ;


/* ugly_runtime api */
UGLY_IFACE_V ugly_runtime*	ugly_runtime_new			(ugly_context *ctx, ugly_language *lang) ; 
const_char*					ugly_runtime_to_string 		(const ugly_runtime *rt) ;
ugly_language*				ugly_runtime_get_language	(const ugly_runtime *rt) ;
UGLY_IFACE void 			ugly_runtime_load_library	(ugly_context *ctx, ugly_runtime *rt, const char *lib) ;
UGLY_IFACE ugly_value*		ugly_runtime_call_function	(ugly_context *ctx, ugly_runtime *rt, ugly_type rtype, const char *func, ugly_value **args, int nb_args, const char *hint) ;


/* ugly_object api */
ugly_runtime*				ugly_object_get_runtime		(const ugly_object *obj) ;
const_char*					ugly_object_get_class		(const ugly_object *obj) ;
void*						ugly_object_get_handle		(const ugly_object *obj) ;
const_char*					ugly_object_to_string		(ugly_object* obj) ;
ugly_object*				ugly_object_wrap			(ugly_context *ctx, ugly_runtime* rt, const char *class, void *ref) ;
UGLY_IFACE ugly_object*		ugly_object_new				(ugly_context *ctx, ugly_runtime* rt, const char *class, ugly_value **args, int nb_args, const char *hint) ;
UGLY_IFACE ugly_value*		ugly_object_call_method		(ugly_context *ctx, ugly_object *obj, ugly_type rtype, const char *method, ugly_value **args, int nb_args, const char *hint) ;
UGLY_IFACE void				ugly_object_delete			(ugly_context *ctx, ugly_object *obj) ;

