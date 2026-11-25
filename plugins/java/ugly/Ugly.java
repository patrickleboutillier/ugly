package ugly;

/**
 * Other languages from Java code running under UGLY:
 *
 *   Object json = Ugly.module("python", "json");
 *   Object s = ((Foreign)json).invoke("dumps", java.util.List.of(1, 2, 3));
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
