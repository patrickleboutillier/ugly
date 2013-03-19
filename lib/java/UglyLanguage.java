package ugly ;


import com.sun.jna.Native ;
import com.sun.jna.Pointer ;


public class UglyLanguage {
	Pointer ptr ;

    static {
        Native.register("ugly") ;
    }

	static native Pointer ugly_language_init(Pointer ctx, String name) ;
	static native String ugly_language_get_name(Pointer lang) ;
	static native String ugly_language_get_dir(Pointer lang) ;

	static public UglyLanguage init(UglyContext ctx, String name){
		Pointer ptr = ugly_language_init(ctx.ptr, name) ;
		UglyLanguage l = new UglyLanguage(ptr) ;
		return l ;
	}

	UglyLanguage(Pointer ctx){
		ptr = ctx ;
	}

	static public UglyLanguage wrap(Pointer lang){
		UglyLanguage l = new UglyLanguage(lang) ;
		return l ;
	}

	public String getName(){
		return ugly_language_get_name(this.ptr) ;
	}

	public String getDir(){
		return ugly_language_get_dir(this.ptr) ;
	}
} ;

