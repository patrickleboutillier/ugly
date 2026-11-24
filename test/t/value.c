/* ugly_value: the tagged union that carries every value across a language boundary. */
#include "ugly_test.h"


int main(void){
	{
		ugly_value *v = ugly_value_new_bool(1) ;
		is_long(ugly_value_get_type(v), UGLY_BOOL, "bool: type") ;
		ok(ugly_value_get_bool(v) == 1, "bool: true") ;
		is_string(ugly_value_to_string(v), "BOOL(true)", "bool: to_string") ;
		ugly_value_delete(v) ;

		v = ugly_value_new_bool(42) ;
		ok(ugly_value_get_bool(v) == 1, "bool: any non-zero value is normalized to 1") ;
		ugly_value_delete(v) ;

		v = ugly_value_new_bool(0) ;
		ok(ugly_value_get_bool(v) == 0, "bool: false") ;
		is_string(ugly_value_to_string(v), "BOOL(false)", "bool: to_string false") ;
		ugly_value_delete(v) ;
	}

	{
		ugly_value *v = ugly_value_new_long(42L) ;
		is_long(ugly_value_get_type(v), UGLY_LONG, "long: type") ;
		is_long(ugly_value_get_long(v), 42L, "long: value") ;
		is_string(ugly_value_to_string(v), "LONG(42)", "long: to_string") ;
		ugly_value_delete(v) ;

		v = ugly_value_new_long(-7L) ;
		is_long(ugly_value_get_long(v), -7L, "long: negative value") ;
		ugly_value_delete(v) ;
	}

	{
		ugly_value *v = ugly_value_new_double(42.6) ;
		is_long(ugly_value_get_type(v), UGLY_DOUBLE, "double: type") ;
		is_double(ugly_value_get_double(v), 42.6, "double: value") ;
		ugly_value_delete(v) ;
	}

	{
		ugly_value *v = ugly_value_new_char('x') ;
		is_long(ugly_value_get_type(v), UGLY_CHAR, "char: type") ;
		ok(ugly_value_get_char(v) == 'x', "char: value") ;
		is_string(ugly_value_to_string(v), "CHAR(x)", "char: to_string") ;
		ugly_value_delete(v) ;
	}

	{
		ugly_value *v = ugly_value_new_string("some string") ;
		is_long(ugly_value_get_type(v), UGLY_STRING, "string: type") ;
		is_string(ugly_value_get_string(v), "some string", "string: value") ;
		is_string(ugly_value_to_string(v), "STRING(some string)", "string: to_string") ;
		ugly_value_delete(v) ;

		char buf[] = "copied" ;
		v = ugly_value_new_string(buf) ;
		buf[0] = 'X' ;
		is_string(ugly_value_get_string(v), "copied", "string: value is a copy, not a reference") ;
		ugly_value_delete(v) ;

		v = ugly_value_new_string("") ;
		is_string(ugly_value_get_string(v), "", "string: empty") ;
		ugly_value_delete(v) ;

		v = ugly_value_new_string(NULL) ;
		is_string(ugly_value_get_string(v), NULL, "string: NULL") ;
		is_string(ugly_value_to_string(v), "STRING(NULL)", "string: to_string NULL") ;
		ugly_value_delete(v) ;
	}

	{
		ugly_value *v = ugly_value_new_object(NULL) ;
		is_long(ugly_value_get_type(v), UGLY_OBJECT, "object: type") ;
		ok(ugly_value_get_object(v) == NULL, "object: NULL") ;
		is_string(ugly_value_to_string(v), "OBJECT(NULL)", "object: to_string NULL") ;
		ugly_value_delete(v) ;
	}

	ugly_value_delete(NULL) ;
	ok(1, "deleting a NULL value is a no-op") ;

	{
		/* ugly_debug_args: joined with commas, and cut with "..." when too long. */
		ugly_value *args[] = { ugly_value_new_long(1), ugly_value_new_string("two") } ;
		is_string(ugly_debug_args(args, 2), "LONG(1),STRING(two)", "debug_args: values joined with commas") ;
		is_string(ugly_debug_args(args, 0), "", "debug_args: no args") ;
		ugly_value_delete(args[0]) ;
		ugly_value_delete(args[1]) ;

		char big[300] ;
		memset(big, 'x', sizeof(big) - 1) ;
		big[sizeof(big) - 1] = '\0' ;
		ugly_value *many[10] ;
		int i ;
		for (i = 0 ; i < 10 ; i++){
			many[i] = ugly_value_new_string(big) ;
		}
		const char *s = ugly_debug_args(many, 10) ;
		size_t len = strlen(s) ;
		ok(len < 1024, "debug_args: long output stays within its buffer (%zu chars)", len) ;
		ok((len >= 3)&&(strcmp(s + len - 3, "...") == 0), "debug_args: long output ends with ...") ;
		for (i = 0 ; i < 10 ; i++){
			ugly_value_delete(many[i]) ;
		}
	}

	return done_testing() ;
}
