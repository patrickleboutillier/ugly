package ugly;

import java.lang.ref.Cleaner;
import java.lang.ref.WeakReference;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;
import java.util.NoSuchElementException;

/**
 * A value from another language: one owned UGLY handle, released when this
 * object is collected. There is one Foreign per foreign value (== holds).
 * Its methods go through the core with that language's own rules.
 */
public final class Foreign implements Iterable<Object> {
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
		cleaner.register(this, () -> { release(handle); forget(handle); });
	}

	/** Drops a collected Foreign's cache entry, unless a new Foreign took the handle's place meanwhile. */
	private static synchronized void forget(long handle){
		WeakReference<Foreign> ref = cache.get(handle);
		if ((ref != null)&&(ref.get() == null)) cache.remove(handle);
	}

	/** Live cache entries, for the tests. */
	static synchronized int cached(){
		return cache.size();
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

	/** Calls it; an {@link Ugly#kw} last gives keyword arguments. */
	public Object call(Object... args){
		Kw.Split s = new Kw.Split(args);
		return s.plain() ? call(handle, args) : callOpts(handle, s.args, s.names(), s.values(), s.hint(), s.context());
	}

	/** Invokes a method, with the foreign language's receiver rules; an {@link Ugly#kw} last gives keyword arguments. */
	public Object invoke(String method, Object... args){
		Kw.Split s = new Kw.Split(args);
		return s.plain() ? invoke(handle, method, args)
			: invokeOpts(handle, method, s.args, s.names(), s.values(), s.hint(), s.context());
	}
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

	/** Its own language's string for it (Python's str(), Lua's tostring); ugly.Foreign(n) for a value it has none for. */
	@Override
	public String toString(){
		String s = toStr(handle);
		return (s != null ? s : "ugly.Foreign(" + handle + ")");
	}

	/**
	 * for-each, and anything taking an Iterable: the value's own iteration
	 * (contract §2), one value per step, so a Python generator runs only as
	 * far as the loop goes. A value its language doesn't iterate but that
	 * has elements (a Lua table) is walked by index.
	 */
	@Override
	@SuppressWarnings("unchecked")
	public Iterator<Object> iterator(){
		Object it = iter(handle);
		if (it == null){
			if ((traits() & Bridge.ELEMENTS) == 0) throw new UnsupportedOperationException("ugly: the foreign value is not iterable");
			return new Iterator<Object>(){
				int i = 0;
				final int n = length();
				@Override public boolean hasNext(){ return i < n; }
				@Override public Object next(){
					if (i >= n) throw new NoSuchElementException();
					return get(i++);
				}
			};
		}
		if (it instanceof Iterator) return (Iterator<Object>)it;     // a Java iterator, come home
		final Foreign f = (Foreign)it;
		return new Iterator<Object>(){
			final Object[] slot = new Object[1];
			int state = 0;     // 0: not asked yet, 1: a value in slot, 2: the end
			@Override public boolean hasNext(){
				if (state == 0) state = (Foreign.next(f.handle, slot) ? 1 : 2);
				return state == 1;
			}
			@Override public Object next(){
				if (! hasNext()) throw new NoSuchElementException();
				Object v = slot[0];
				slot[0] = null;
				state = 0;
				return v;
			}
		};
	}

	/* Natives, registered by the plugin (RegisterNatives). */
	static native Object call(long h, Object[] args);
	static native Object invoke(long h, String method, Object[] args);
	static native Object callOpts(long h, Object[] args, String[] names, Object[] values, String hint, String context);
	static native Object invokeOpts(long h, String method, Object[] args, String[] names, Object[] values,
		String hint, String context);
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
	static native void configure(String lang, String key, String value);
	static native Object iter(long h);
	static native boolean next(long h, Object[] slot);
	static native String toStr(long h);
}
