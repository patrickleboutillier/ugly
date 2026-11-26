# Benchmarks

What a call through UGLY costs, measured by
[tests/bench.c](../tests/bench.c). It loads every language built into one
process, with their `conformance` fixtures, and measures:

- **C calling each language:** `ugly_call` on the language's `echo`, with an
  int, a 16-byte string, and a handle (the language's own object, which
  goes home and comes back). Beside it, the same call made natively: the
  fixture's `bench_self(n)`, a loop in the language calling its own `echo`.
- **Each language calling each other one:** `A.bench(B.echo, n)`, a loop in
  A calling B's `echo` through a proxy, for every ordered pair. On the
  diagonal, a language calls its own function passed to it as a value.
- **Threads:** total calls per second from 1 to N threads at once, each
  calling a C callable (only the core is involved), its own Lua runtime, or
  the one Python.

Each measurement doubles its count until a run takes 0.3 s
(`BENCH_SECONDS`), so the first rounds also warm up the JITs. Java runs
without `-Xcheck:jni`, which the tests use and which slows every JNI call.

```sh
meson test -C build --benchmark --verbose
```

## Results

2026-11-26 (after the fixes below): Intel Core i7-1365U, 6 CPUs under WSL2 (Linux 6.6), GCC 11.4,
a `debugoptimized` build (`-O2 -g`); Lua 5.4.4, CPython 3.10.12, QuickJS
2026-06-04, Perl 5.34.0, OpenJDK 21.0.13, .NET 10.0.15.

### C calling each language (ns per call to `echo`)

| Language | int | 16-byte string | its own object | natively, in the language |
|---|---|---|---|---|
| mock | 96.5 ns | 137 ns | 165 ns | 13.4 ns |
| lua | 61.0 ns | 124 ns | 163 ns | 8.4 ns |
| python | 118 ns | 147 ns | 191 ns | 43.5 ns |
| js | 80.7 ns | 117 ns | 169 ns | 18.7 ns |
| perl5 | 157 ns | 255 ns | 283 ns | 28.3 ns |
| java | 437 ns | 626 ns | 787 ns | 0.0 ns |
| dotnet | 123 ns | 263 ns | 251 ns | 0.1 ns |

The JITs inline a native `echo` away entirely, hence the zeros.

### Each language calling each other one (ns per call: row's loop calling column's `echo`)

| Caller \ callee | mock | lua | python | js | perl5 | java | dotnet |
|---|---|---|---|---|---|---|---|
| mock | 20.0 ns | 141 ns | 205 ns | 169 ns | 229 ns | 466 ns | 193 ns |
| lua | 190 ns | 9.1 ns | 262 ns | 393 ns | 324 ns | 600 ns | 260 ns |
| python | 289 ns | 220 ns | 39.9 ns | 241 ns | 352 ns | 676 ns | 337 ns |
| js | 231 ns | 135 ns | 211 ns | 20.9 ns | 254 ns | 540 ns | 215 ns |
| perl5 | 421 ns | 417 ns | 502 ns | 388 ns | 31.9 ns | 880 ns | 874 ns |
| java | 392 ns | 415 ns | 422 ns | 355 ns | 481 ns | 65.2 ns | 476 ns |
| dotnet | 281 ns | 264 ns | 415 ns | 301 ns | 456 ns | 700 ns | 76.7 ns |

### Threads (total calls per second)

| Each thread calls | 1 thread | 2 threads | 4 threads | 6 threads |
|---|---|---|---|---|
| a C callable (the core alone) | 20.32 M/s | 40.84 M/s | 54.31 M/s | 63.05 M/s |
| its own Lua runtime | 12.17 M/s | 24.58 M/s | 34.10 M/s | 34.94 M/s |
| the one Python (the GIL) | 4.91 M/s | 2.00 M/s | 0.28 M/s | 0.09 M/s |

## Reading them

- **A call across languages costs 100 to 300 ns** between the
  interpreters (Lua, Python, JS, Perl, .NET), a few times a native call in
  the slower ones and 10 to 30 times in Lua. It suits calls that do some
  work, not a tight inner loop.
- **Java is the slowest callee, at 450 to 900 ns**: every call still makes
  about five JNI calls into Java (boxing the argument, the call itself,
  classifying the result, unboxing it, the argument array), at 30 to 60 ns
  of VM transition each. Fewer would take one Java call doing all of it, or
  Panama's faster upcalls.
- **Python doesn't scale with threads, and slows down.** Every call into
  Python takes the GIL, and with several threads calling at once the GIL
  changes hands on every call: about 8 µs per call with 4 or more threads.
  That's CPython's design (a free-threaded CPython would change it). Keep
  calls into Python on few threads, or make each one do more.
- **The core scales with threads**: calls into separate runtimes don't wait
  for each other.

## What the first run found (2026-11-26)

The first run, before these fixes, measured:

| | before | after |
|---|---|---|
| C callables, 1 → 6 threads | 13.7 → **3.6** M/s | 20.3 → **63.1** M/s |
| Lua (a runtime per thread), 1 → 6 threads | 11.1 → **3.3** M/s | 12.2 → **34.9** M/s |
| C → .NET, int | 358 ns | 123 ns |
| C → Java, int (without `-Xcheck:jni`) | 1027 ns | 437 ns |
| Perl calling Lua, Python or JS | 730 to 1750 ns | 390 to 500 ns |
| C → Lua, int | 84 ns | 61 ns |

- **The core's lock.** Every call took the handle table's one lock twice:
  to look up the callee, and to check the runtime's queue of deferred
  releases. With several threads, total throughput fell as threads were
  added. The queue is now checked without the lock (an atomic count), and
  a lookup reads the table without it: slots live in chunks that never
  move, and each slot has a sequence number that writers bump around a
  change, so a lookup retries if one happened (a seqlock). ThreadSanitizer
  finds no race, in the threads tests or under the benchmark.
- **.NET and Java resolved every call from scratch**, by reflection: the
  methods of that name, their parameters, and scoring each overload. They
  now cache the candidates per (type, name) and the overload chosen per
  argument signature.
- **Java, profiled with perf**: besides the reflection, every call looked up
  `java.lang.Object` with `FindClass`, which walks the Java stack to find
  its caller's class loader (12% of the call), and classified the result
  with up to eleven `IsInstanceOf` calls (11%). The class is now looked up
  once, and one call to `Bridge.kind` classifies a result. 610 to 437 ns.
- **Perl made a closure for every call** of a foreign function
  (`$f->(...)`, through the `&{}` overload). The code ref is now made once
  per proxy, over a second proxy that holds its own reference, so it
  neither keeps the first alive nor dies with it.
