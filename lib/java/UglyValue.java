package ugly ;


import com.sun.jna.Native ;
import com.sun.jna.Pointer ;


public class UglyValue {
	Pointer ptr ;

    static {
        Native.register("ugly") ;
    }

	static native Pointer ugly_value_new_bool(byte val) ;
	static native Pointer ugly_value_new_long(long val) ;
	static native Pointer ugly_value_new_double(double val) ;
	static native Pointer ugly_value_new_char(char val) ;
	static native Pointer ugly_value_new_string(String val) ;
	// static native Pointer ugly_value_new_object(Pointer val) ;
	static native void ugly_value_delete(Pointer val) ;
	static native int ugly_value_get_type(Pointer val) ;
	static native byte ugly_value_get_bool(Pointer val) ;
	static native long ugly_value_get_long(Pointer val) ;
	static native double ugly_value_get_double(Pointer val) ;
	static native char ugly_value_get_char(Pointer val) ;
	static native String ugly_value_get_string(Pointer val) ;
	// static native Pointer ugly_value_get_object(Pointer val) ;
	static native String ugly_value_to_string(Pointer val) ;


	public UglyValue(boolean b){
		ptr = ugly_value_new_bool((byte)(b ? 1 : 0)) ;
	}

	public UglyValue(long l){
		ptr = ugly_value_new_long(l) ;
	}

	public UglyValue(double d){
		ptr = ugly_value_new_double(d) ;
	}

	public UglyValue(char c){
		ptr = ugly_value_new_char(c) ;
	}

	public UglyValue(String s){
		ptr = ugly_value_new_string(s) ;
	}

	private UglyValue(){
	}

	public static UglyValue wrap(Pointer val){
		UglyValue v = new UglyValue() ;
		v.ptr = val ;
		return v ;
	}

	public Pointer getPtr(){
		return ptr ;
	}

	public UglyType getType(){
		int t = ugly_value_get_type(this.ptr) ;
		return UglyType.wrap(t) ;
	}

	public boolean getBool(){
		int b = ugly_value_get_bool(this.ptr) ;
		return (b != 0 ? true : false) ;
	}

	public long getLong(){
		return ugly_value_get_long(this.ptr) ;
	}

	public double getDouble(){
		return ugly_value_get_double(this.ptr) ;
	}

	public char getChar(){
		return ugly_value_get_char(this.ptr) ;
	}

	public String getString(){
		return ugly_value_get_string(this.ptr) ;
	}

	public String toString(){
		return ugly_value_to_string(this.ptr) ;
	}

	public void delete(){
		ugly_value_delete(this.ptr) ;
	}
} ;


