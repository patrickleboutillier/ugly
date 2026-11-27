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
import java.util.Iterator;
import java.util.NoSuchElementException;
import java.util.Set;
import java.util.stream.Stream;
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
		@Override public String toString(){ return "counter(" + count + ")"; }
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

	/* Benchmarks (tests/bench.c): f(i) n times, and our own echo n times (keeping the results, so the JIT can't drop the calls). */
	public static long bench(Function<Object, Object> f, long n){
		long kept = 0;
		for (long i = 0 ; i < n ; i++) if (f.apply(i) != null) kept++;
		return kept;
	}
	public static long bench_self(long n){
		long kept = 0;
		for (long i = 0 ; i < n ; i++) if (echo(i) != null) kept++;
		return kept;
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

	/* Iteration (contract §2): iterators that run only as far as asked, and a stream, closed when released. */
	private static long producedCount = 0;
	private static volatile long closedCount = 0;

	public static Iterator<Long> count_to(long n){
		producedCount = 0;
		return new Iterator<Long>(){
			long i = 0;
			@Override public boolean hasNext(){ return i < n; }
			@Override public Long next(){
				if (i >= n) throw new NoSuchElementException();
				producedCount++;
				return i++;
			}
		};
	}

	public static Stream<Long> naturals(){
		return Stream.iterate(0L, i -> i + 1).onClose(() -> closedCount++);
	}

	public static Iterator<Long> fail_after(long n){
		return new Iterator<Long>(){
			long i = 0;
			@Override public boolean hasNext(){ return true; }
			@Override public Long next(){
				if (i >= n) throw new ConformanceError("fail_after(" + n + ")");
				return i++;
			}
		};
	}

	public static long produced(){ return producedCount; }

	/* for-each takes an Iterable; an iterator coming home is looped over as it is. */
	private static Iterable<?> each(Object xs){
		if (xs instanceof Iterator) return () -> (Iterator<Object>)xs;
		return (Iterable<?>)xs;
	}

	public static long sum_iter(Object xs){
		long total = 0;
		for (Object x : each(xs)) total += ((Number)x).longValue();
		return total;
	}

	public static ArrayList<Object> take_iter(Object xs, long n){
		ArrayList<Object> taken = new ArrayList<>();
		if (n <= 0) return taken;
		for (Object x : each(xs)){
			taken.add(x);
			if (taken.size() >= n) break;
		}
		return taken;
	}

	public static String stringify(Object x){ return String.valueOf(x); }

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
		return finalizedCount + closedCount;
	}
}
