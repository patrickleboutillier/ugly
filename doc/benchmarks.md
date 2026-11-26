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

2026-11-26: Intel Core i7-1365U, 6 CPUs under WSL2 (Linux 6.6), GCC 11.4,
a `debugoptimized` build (`-O2 -g`); Lua 5.4.4, CPython 3.10.12, QuickJS
2026-06-04, Perl 5.34.0, OpenJDK 21.0.13, .NET 10.0.15.

### C calling each language (ns per call to `echo`)

| Language | int | 16-byte string | its own object | natively, in the language |
|---|---|---|---|---|
| mock | 97.7 ns | 138 ns | 171 ns | 13.0 ns |
| lua | 59.4 ns | 119 ns | 160 ns | 8.8 ns |
| python | 115 ns | 144 ns | 194 ns | 41.5 ns |
| js | 81.8 ns | 128 ns | 195 ns | 22.3 ns |
| perl5 | 183 ns | 294 ns | 322 ns | 30.8 ns |
| java | 610 ns | 697 ns | 954 ns | 0.0 ns |
| dotnet | 145 ns | 292 ns | 302 ns | 0.1 ns |

The JITs inline a native `echo` away entirely, hence the zeros.

### Each language calling each other one (ns per call: row's loop calling column's `echo`)

| Caller \ callee | mock | lua | python | js | perl5 | java | dotnet |
|---|---|---|---|---|---|---|---|
| mock | 19.5 ns | 143 ns | 230 ns | 169 ns | 253 ns | 698 ns | 199 ns |
| lua | 203 ns | 9.6 ns | 268 ns | 415 ns | 384 ns | 850 ns | 291 ns |
| python | 265 ns | 231 ns | 42.7 ns | 265 ns | 376 ns | 824 ns | 316 ns |
| js | 188 ns | 143 ns | 221 ns | 20.9 ns | 267 ns | 723 ns | 237 ns |
| perl5 | 391 ns | 388 ns | 504 ns | 425 ns | 31.2 ns | 1056 ns | 540 ns |
| java | 376 ns | 379 ns | 438 ns | 569 ns | 938 ns | 68.5 ns | 445 ns |
| dotnet | 296 ns | 255 ns | 382 ns | 298 ns | 476 ns | 938 ns | 84.3 ns |

### Threads (total calls per second)

| Each thread calls | 1 thread | 2 threads | 4 threads | 6 threads |
|---|---|---|---|---|
| a C callable (the core alone) | 23.87 M/s | 44.80 M/s | 52.44 M/s | 67.62 M/s |
| its own Lua runtime | 13.18 M/s | 24.52 M/s | 31.13 M/s | 38.44 M/s |
| the one Python (the GIL) | 4.87 M/s | 1.89 M/s | 0.23 M/s | 0.12 M/s |

## Reading them

- **A call across languages costs 100 to 300 ns** between the
  interpreters (Lua, Python, JS, Perl, .NET), a few times a native call in
  the slower ones and 10 to 30 times in Lua. It suits calls that do some
  work, not a tight inner loop.
- **Java is the slowest callee, at 600 to 1000 ns**: every call crosses JNI
  several times (a local frame, an argument array, boxing, reflection, the
  result's checks). Not yet profiled.
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
| C callables, 1 → 6 threads | 13.7 → **3.6** M/s | 23.9 → **67.6** M/s |
| Lua (a runtime per thread), 1 → 6 threads | 11.1 → **3.3** M/s | 13.2 → **38.4** M/s |
| C → .NET, int | 358 ns | 145 ns |
| C → Java, int (without `-Xcheck:jni`) | 1027 ns | 610 ns |
| Perl calling another language | 730 to 1750 ns | 390 to 540 ns |
| C → Lua, int | 84 ns | 59 ns |

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
- **Perl made a closure for every call** of a foreign function
  (`$f->(...)`, through the `&{}` overload). The code ref is now made once
  per proxy, over a second proxy that holds its own reference, so it
  neither keeps the first alive nor dies with it.
