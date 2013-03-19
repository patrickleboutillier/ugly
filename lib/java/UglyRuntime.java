package ugly ;


import com.sun.jna.Native ;
import com.sun.jna.Pointer ;


public class UglyRuntime {
	Pointer ptr ;

    static {
        Native.register("ugly") ;
    }

	static native Pointer ugly_runtime_new(Pointer ctx, Pointer lang) ;
	static native void ugly_runtime_load_library(Pointer ctx, Pointer rt, String lib) ;
	static native Pointer ugly_runtime_get_language(Pointer rt) ;
	static native String ugly_runtime_to_string(Pointer rt) ;
	static native Pointer ugly_runtime_call_function(Pointer ctx, Pointer rt, UglyType rtype, 
		String func, Pointer args, int nb_args, String hint) ;

	public UglyRuntime(UglyContext ctx, UglyLanguage lang){
		ptr = ugly_runtime_new(ctx.ptr, lang.ptr) ;
	}

	private UglyRuntime(Pointer lang){
		ptr = lang ;
	}

	static public UglyRuntime warp(Pointer ctx){
		UglyRuntime r = new UglyRuntime(ctx) ;
		return r ;
	}

	public void loadLibrary(UglyContext ctx, String lib){
		ugly_runtime_load_library(ctx.ptr, this.ptr, lib) ;
	}

	public UglyLanguage getLanguage(){
		return UglyLanguage.wrap(ugly_runtime_get_language(this.ptr)) ;
	}

	public String toString(){
		return ugly_runtime_to_string(this.ptr) ;
	}

	/*
	public UglyValue callFunction(UglyContext ctx, UglyType rtype, String func, UglyValue args[], String hint){
		Pointer a = new Memory(args.size()) ;
		for (int i = 0 ; i < args.size() ; i++){
			a.setLong(i * Native.getNativeSize(Long.TYPE)
		}
		
		Pointer v = ugly_runtime_call_function(ctx.ptr, this.ptr, rtype.getNo(), func, , args.size(), hint) ;
		return UglyValue.wrap(v) ;
	}
	*/
} ;

