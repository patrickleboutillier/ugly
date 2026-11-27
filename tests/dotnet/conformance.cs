// The conformance fixtures (doc/contract.md section 9), in C#.
//
// The module is the static class conformance; its public nested types are
// members (Point is instantiable). Type names are simple type names:
// counter, list. Exceptions are ConformanceError.
using System.Collections;
using System.Runtime.ExceptionServices;
using Ugly;

public static class conformance {
    static long finalizedCount;

    public sealed class list : List<object?> {
        ~list() { Interlocked.Increment(ref finalizedCount); }
    }

    public sealed class counter {
        public long count;
        public readonly list items = new();
        public long inc() { count++; items.Add(count); return count; }
        public override string ToString() => $"counter({count})";
        ~counter() { Interlocked.Increment(ref finalizedCount); }
    }

    public sealed class ConformanceError(string message) : Exception(message) {
        public readonly string message = message;
    }

    public sealed class Point(double x, double y) {
        public double x = x, y = y;
        public double norm2() => x * x + y * y;
    }

    // Call options (contract §11): keyword arguments by parameter name; defaults for the rest.
    public sealed class KwBox(long value = 0) {
        public long value = value;
        public long add(long n = 1) => value + n;
    }

    public static string kw(object? a, long b = 0, long c = 0) => $"{a} {b} {c}";

    // Overloads, for the hint call option.
    public static string over(int x) => "int";
    public static string over(long x) => "long";
    public static string over(object x) => "object";

    public static object? echo(object? x) => x;

    public static string kind_of(object? x) => x switch {
        null => "null",
        bool => "bool",
        long or int or short or sbyte or byte or ushort or uint or ulong => "int",
        double or float => "double",
        string or char => "string",
        byte[] => "bytes",
        _ when Interop.IsForeign(x) => "proxy",
        _ => x.GetType().Name,
    };

    // Benchmarks (tests/bench.c): f(i) n times, and our own echo n times (keeping the results, so the JIT can't drop the calls).
    public static long bench(Func<object?, object?> f, long n) {
        long kept = 0;
        for (long i = 0; i < n; i++) if (f(i) != null) kept++;
        return kept;
    }
    public static long bench_self(long n) {
        long kept = 0;
        for (long i = 0; i < n; i++) if (echo(i) != null) kept++;
        return kept;
    }

    public static object make_counter() => new counter();
    public static object? apply(Func<object?, object?> f, object? x) => f(x);
    public static object? call_twice(Func<object?, object?> f, object? x) => f(f(x));
    public static object? raise_error(string msg) => throw new ConformanceError(msg);

    // Returns the exception object: ours as is, a foreign one through its payload.
    public static object? catch_and_return(Func<object?> f) {
        try {
            return f();
        }
        catch (ForeignException e) {
            return e.Payload;
        }
        catch (Exception e) {
            return e;
        }
    }

    public static object? rethrow(object? e) {
        if (e is Exception ex) ExceptionDispatchInfo.Throw(ex);
        throw new ForeignException(e);
    }

    // Iteration (contract §2): 'yield return' methods, run only as far as asked.
    static long producedCount;

    public static IEnumerable<long> count_to(long n) {
        producedCount = 0;
        return Counting(n);
    }

    static IEnumerable<long> Counting(long n) {
        for (long i = 0; i < n; i++) {
            producedCount++;
            yield return i;
        }
    }

    public static IEnumerable<long> naturals() {
        try {
            for (long i = 0; ; i++) yield return i;
        }
        finally {     // disposed of when released early
            Interlocked.Increment(ref finalizedCount);
        }
    }

    public static IEnumerable<long> fail_after(long n) {
        for (long i = 0; i < n; i++) yield return i;
        throw new ConformanceError($"fail_after({n})");
    }

    public static long produced() => producedCount;

    public static long sum_iter(IEnumerable xs) {
        long total = 0;
        foreach (object? x in xs) total += Convert.ToInt64(x);
        return total;
    }

    public static List<object?> take_iter(IEnumerable xs, long n) {
        var taken = new List<object?>();
        if (n <= 0) return taken;
        foreach (object? x in xs) {
            taken.Add(x);
            if (taken.Count >= n) break;
        }
        return taken;
    }

    public static string stringify(object? x) => x?.ToString() ?? "null";

    public static long finalized() {
        for (int round = 0; round < 3; round++) {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
        return Interlocked.Read(ref finalizedCount);
    }
}
