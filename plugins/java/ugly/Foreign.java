package ugly;

import java.lang.ref.Cleaner;
import java.lang.ref.WeakReference;
import java.util.HashMap;
import java.util.Map;

/**
 * A value from another language: one owned UGLY handle, released when this
 * object is collected. There is one Foreign per foreign value (== holds).
 * Its methods go through the core with that language's own rules.
 */
public final class Foreign {
	private static final Cleaner cleaner = Cleaner.create();
	private static final Map<Long, WeakReference<Foreign>> cache = new HashMap<>();

	/*
	 * Under a plain java program (the binding), our natives come from the
	 * library libugly_jni.so (-Dugly.library=<path> names it explicitly),
	 * whose JNI_OnLoad attaches UGLY to this JVM, on this thread. When UGLY
	 * started the JVM (the plugin), it registered them already and said so.
	 */
	static {
		if (! Boolean.getBoolean("ugly.embedded")){
			String lib = System.getProperty("ugly.library");
			if (lib != null) System.load(lib);
			else System.loadLibrary("ugly_jni");
		}
	}

	final long handle;

	private Foreign(long handle){
		this.handle = handle;
		cleaner.register(this, () -> release(handle));
	}

	/** The Foreign for an owned handle: the cached one (releasing the extra reference), or a new one. */
	static synchronized Foreign of(long handle){
		WeakReference<Foreign> ref = cache.get(handle);
		Foreign f = (ref != null ? ref.get() : null);
		if (f != null){
			release(handle);
			return f;
		}
		f = new Foreign(handle);
		cache.put(handle, new WeakReference<>(f));
		return f;
	}

	public Object call(Object... args){ return call(handle, args); }
	public Object invoke(String method, Object... args){ return invoke(handle, method, args); }
	public Object get(String member){ return get(handle, member); }
	public void set(String member, Object value){ set(handle, member, value); }
	public Object get(int index){ return getIndex(handle, index); }
	public void set(int index, Object value){ setIndex(handle, index, value); }
	public int length(){ return length(handle); }
	public String[] names(){ return names(handle); }
	public int traits(){ return traits(handle); }

	@Override
	public boolean equals(Object o){
		return (o instanceof Foreign) && same(handle, ((Foreign)o).handle);
	}

	@Override
	public int hashCode(){
		return Long.hashCode(handle);
	}

	@Override
	public String toString(){
		return "ugly.Foreign(" + handle + ")";
	}

	/* Natives, registered by the plugin (RegisterNatives). */
	static native Object call(long h, Object[] args);
	static native Object invoke(long h, String method, Object[] args);
	static native Object get(long h, String member);
	static native void set(long h, String member, Object value);
	static native Object getIndex(long h, int index);
	static native void setIndex(long h, int index, Object value);
	static native int length(long h);
	static native String[] names(long h);
	static native int traits(long h);
	static native boolean same(long a, long b);
	static native void release(long h);
	static native void freeError(long err);
	static native Object module(String lang, String name);
	static native Object eval(String lang, String code);
}
