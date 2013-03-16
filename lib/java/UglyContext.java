package ugly ;


import com.sun.jna.Native ;
import com.sun.jna.Pointer ;


public class UglyContext {
	Pointer handle ;

    static {
        Native.register("ugly") ;
    }

	public static native Pointer ugly_context_new() ;

	public UglyContext(){
		handle = ugly_context_new() ;
	}

	public UglyContext(Pointer p){
		handle = p ;
	}

//ugly_error                  ugly_context_get_error      (const ugly_context *ctx) ;
//const_char*                 ugly_context_get_error_msg  (const ugly_context *ctx) ;
//void                        ugly_context_set_error      (ugly_context *ctx, ugly_error error, const char *fmt, ...) ;
//void                        ugly_context_clear_error    (ugly_context *ctx) ;
//void                        ugly_context_delete         (ugly_context *ctx) ;
} ;


