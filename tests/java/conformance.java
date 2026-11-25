// The conformance fixtures (doc/contract.md section 9), in Java.
//
// The module is the class conformance; its public nested classes are
// members (Point is instantiable). Type names are simple class names:
// counter, list. Exceptions are ConformanceError.

import java.lang.ref.PhantomReference;
import java.lang.ref.Reference;
import java.lang.ref.ReferenceQueue;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.Set;
import java.util.function.Function;
import java.util.function.Supplier;

import ugly.ForeignException;
import ugly.Ugly;

public final class conformance {
	private conformance(){ }

	/* Finalization, counted for counters and their item lists: phantom references, collected on demand. */
	private static final ReferenceQueue<Object> queue = new ReferenceQueue<>();
	private static final Set<Reference<?>> tracked = Collections.synchronizedSet(new HashSet<>());
	private static long finalizedCount = 0;

	static void track(Object o){
		tracked.add(new PhantomReference<>(o, queue));
	}

	public static final class list extends ArrayList<Object> {
		private static final long serialVersionUID = 1L;
		public list(){ track(this); }
	}

	public static final class counter {
		public long count = 0;
		public final list items = new list();
		public counter(){ track(this); }
		public long inc(){ count++; items.add(count); return count; }
	}

	public static final class ConformanceError extends RuntimeException {
		private static final long serialVersionUID = 1L;
		public final String message;
		public ConformanceError(String message){ super(message); this.message = message; }
	}

	public static final class Point {
		public double x, y;
		public Point(double x, double y){ this.x = x; this.y = y; }
		public double norm2(){ return x * x + y * y; }
	}

	public static Object echo(Object x){ return x; }

	/* Overloads, for the hint call option (contract §11). */
	public static String over(int x){ return "int"; }
	public static String over(long x){ return "long"; }
	public static String over(Object x){ return "java.lang.Object"; }

	public static String kind_of(Object x){
		if (x == null) return "null";
		if (x instanceof Boolean) return "bool";
		if ((x instanceof Long)||(x instanceof Integer)||(x instanceof Short)||(x instanceof Byte)) return "int";
		if ((x instanceof Double)||(x instanceof Float)) return "double";
		if ((x instanceof String)||(x instanceof Character)) return "string";
		if (x instanceof byte[]) return "bytes";
		if (Ugly.isForeign(x)) return "proxy";
		return x.getClass().getSimpleName();
	}

	public static Object make_counter(){ return new counter(); }
	public static Object apply(Function<Object, Object> f, Object x){ return f.apply(x); }
	public static Object call_twice(Function<Object, Object> f, Object x){ return f.apply(f.apply(x)); }
	public static Object raise_error(String msg){ throw new ConformanceError(msg); }

	/** Returns the exception object: ours as is, a foreign one through its payload. */
	public static Object catch_and_return(Supplier<Object> f){
		try {
			return f.get();
		}
		catch (ForeignException e){
			return e.payload();
		}
		catch (RuntimeException e){
			return e;
		}
	}

	public static Object rethrow(Object e){
		if (e instanceof RuntimeException) throw (RuntimeException)e;
		throw new ForeignException(e);
	}

	public static long finalized(){
		for (int round = 0 ; round < 5 ; round++){
			System.gc();
			try {
				Reference<?> r;
				while ((r = queue.remove(20)) != null){
					tracked.remove(r);
					finalizedCount++;
				}
			}
			catch (InterruptedException e){
				Thread.currentThread().interrupt();
			}
		}
		return finalizedCount;
	}
}
