package ugly;

/**
 * Other languages from Java: from code UGLY runs in its JVM, or from a plain
 * java program (ugly.jar on the class path, libugly_jni.so on
 * java.library.path or named by -Dugly.library). In a plain program, the
 * thread that first uses UGLY owns it; other threads get
 * IllegalStateException.
 *
 *   Foreign math = (Foreign)Ugly.module("python", "math");
 *   Object r = math.invoke("sqrt", 16.0);     // 4.0
 *   Object n = Ugly.eval("lua", "return 6 * 7");
 *
 * Values from other languages are Foreign objects; when a method parameter
 * is an interface (Function, Runnable...), a foreign value passed to it is
 * wrapped to implement that interface. Errors from other languages are
 * ForeignException.
 */
public final class Ugly {
	private Ugly(){ }

	/** A module of another language. */
	public static Object module(String lang, String name){
		return Foreign.module(lang, name);
	}

	/** The value of code in another language. */
	public static Object eval(String lang, String code){
		return Foreign.eval(lang, code);
	}

	/** Whether x is a value from another language (or a Java interface wrapping one). */
	public static boolean isForeign(Object x){
		return (x instanceof Foreign)||(Bridge.foreignOf(x) != null);
	}
}
