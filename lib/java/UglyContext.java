package ugly ;


import com.sun.jna.Native ;
import com.sun.jna.Pointer ;


public class UglyContext {
	Pointer ptr ;

    static {
        Native.register("ugly") ;
    }

	static native Pointer ugly_context_new() ;
	static native int ugly_context_has_error(Pointer ctx) ;
	static native int ugly_context_get_error(Pointer ctx) ;
	static native String ugly_context_get_error_msg(Pointer ctx) ;
	static native void ugly_context_set_error(Pointer ctx, int error, String msg) ;
	static native void ugly_context_clear_error(Pointer ctx) ;
	static native void ugly_context_delete(Pointer ctx) ;

	public UglyContext(){
		ptr = ugly_context_new() ;
	}

	private UglyContext(Pointer ctx){
		ptr = ctx ;
	}

	static public UglyContext wrap(Pointer ctx){
		UglyContext c = new UglyContext(ctx) ;
		return c ;
	}

	public boolean hasError(){
		int e = ugly_context_has_error(this.ptr) ;
		return (e != 0 ? true : false) ;
	}

	public UglyError getError(){
		return UglyError.wrap(ugly_context_get_error(this.ptr)) ;
	}

	public String getErrorMsg(){
		return ugly_context_get_error_msg(this.ptr) ;
	}

	public void setError(UglyError err, String msg){
		ugly_context_set_error(this.ptr, err.getNo(), msg) ;
	}

	public void clearError(){
		ugly_context_clear_error(this.ptr) ;
	}

	public void delete(){
		ugly_context_delete(this.ptr) ;
	}
} ;


