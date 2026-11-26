# Writing a plugin

For someone adding a language to UGLY, or maintaining a plugin. The
contract, [doc/contract.md](contract.md), says what a plugin must do, and
[include/ugly_plugin.h](../include/ugly_plugin.h) lists the rules. This
guide is how to get there: the order that has worked, the rules that are
easy to get wrong, and what each plugin so far taught the hard way.

## The order of work

1. **A skeleton first.** The plugin builds, `init` starts the interpreter,
   runtimes come and go, and every operation fails with
   `UGLY_ERR_NOT_SUPPORTED`. The .NET plugin started this way
   (`dotnet: a skeleton plugin`, PR #37). It settles the build, the
   interpreter's startup, signals and CI before any protocol code.
2. **The build.** `plugins/<lang>/meson.build` finds the language (a Meson
   option `-D<lang>=auto|enabled|disabled`; in `auto`, skip quietly if
   something is missing, such as a too-old SDK), builds `ugly_<lang>.so`
   exporting `ugly_plugin_<lang>()`, and appends to `plugins`,
   `plugin_dirs` and `test_plugins`. CI gets the language in the full jobs,
   a job without it, and any setup step it needs (`actions/setup-dotnet`).
3. **The fixtures.** The `conformance` module of contract §9, in the
   language, in `tests/<lang>/`, including `bench` and `bench_self`. Each
   test group then runs against the plugin as `<group>-<lang>`, and
   `pairwise.c` pairs it with every other language.
4. **The protocol**, until the conformance suite and pairwise pass. The
   suite is the contract: an expectation that depends on the language is
   "exact or refused", and a capability the plugin doesn't claim is a SKIP.
5. **The binding**: the same plugin in attach mode, so a program in that
   language can be the host (§8). Then its test, `tests/<lang>/test_binding.*`.
6. **The docs**: the language's column in contract §10 and its rows in §11,
   a guide in `doc/languages/<lang>.md`, a Clarifications entry for what it
   taught, the README, and CLAUDE.md.

The mock plugin (`plugins/mock/`) implements every rule in a few hundred
lines of C, with no interpreter in the way: read it first.

## The rules that are easy to get wrong

- **Arguments are borrowed; results are owned.** A handle argument dies
  when the call returns (the core revokes it). Keep one, in a proxy or a
  closure, only through `ugly_dup`. Strings and bytes in arguments are
  borrowed too; results are copied with `ugly_val_set_string`.
- **Rule 5: unwrap.** A proxy going back to its home language must arrive as
  the original object, never a proxy of a proxy: emit the proxied handle.
  A handle whose home is your runtime resolves (`ugly_resolve`) to your
  native value. A wrapper you made for a foreign value (a Java dynamic
  proxy, a C# delegate) must unwrap too: keep a weak map from wrapper to
  foreign value.
- **Identity.** Export the same object with the same identity key, so the
  core gives back the same handle. A moving GC needs a stable key: an id
  table (Java's `IdentityHashMap`), or a pinned handle whose address is the
  key (.NET's `GCHandle`, counted per object).
- **Rule 7: errors.** A native exception escaping becomes `ugly_raise_native`
  with the exception as payload; a non-exception failure (no such member)
  is `ugly_fail` with its status; a foreign value rethrown is
  `ugly_raise_handle`. A foreign error thrown into your language must come
  back out unchanged if nothing catches it, and if its payload is one of
  your exceptions coming home, throw that object itself. Take your own
  reference to it *before* releasing the payload handle or freeing the
  error: either can drop the last reference (QuickJS's valgrind run found
  this use-after-free).
- **No `longjmp` across the core.** A Lua error or a Perl `die` must be
  caught at your boundary (`lua_pcall`, `G_EVAL`), never unwind through a
  core frame.
- **`release` can come at any time**: in the middle of a call (a finalizer
  ran), and, for `UGLY_CAP_ANY_THREAD`, from any thread. It must be safe to
  re-enter, or defer the work.
- **Errors aren't cleared by cleanup.** `ugly_release`, `ugly_val_release`
  and `ugly_error_payload` leave the ctx's error alone; every call that
  returns a status clears it first.
- **Signals belong to whoever installed them first.** Put back any handler
  your interpreter resets while starting (perl ignores SIGFPE, which the
  JVM needs). A VM that needs fault handlers (the JVM, the CLR) keeps them
  only if they pass faults outside its own code to the handler before it;
  its handlers for the host's signals (SIGINT, SIGTERM, SIGPIPE...) go back
  (the .NET plugin restores five after starting the CLR).
- **stdout belongs to the host.** Send the interpreter's own diagnostics to
  stderr (the JVM's `vfprintf` hook).
- **Options** (`ugly_configure`) are read once, in `init` or `rt_new`;
  declare the keys you take in `options`, and read them with `ugly_option`
  ahead of the environment variables that do the same.

## Threads

- **One thread per runtime**, unless the language can serve every thread
  (`UGLY_CAP_ANY_THREAD`: Python through the GIL, the JVM, the CLR). Then
  every entry point may be called from any thread at once, `release`
  included, and the plugin serializes what needs it.
- **Never hold a lock of your own while calling out** through the core:
  another thread may be calling back in. Python releases the GIL around
  calls out (`OUT()`); C#'s caches use concurrent dictionaries, and its
  proxy cache releases outside its lock.
- Threads your language starts need a ctx of their own:
  `ugly_ctx_thread` makes one, deleted when the thread ends.
- **Test it under ThreadSanitizer** as well as helgrind: helgrind doesn't
  model atomics, TSan does. On recent kernels TSan needs address
  randomization off: `meson setup build-tsan -Db_sanitize=thread`, then
  `meson test -C build-tsan --wrapper "setarch x86_64 -R"`.

## Lifecycle

- `init` runs once per process. A one-way interpreter (CPython, perl's
  `PERL_SYS_INIT3`, the JVM, the CLR) has no `term`, and isn't
  `UGLY_CAP_RESTARTABLE`. Don't tear down what can't come back: after
  `PERL_SYS_TERM`, perl can't start again in the process.
- A runtime's deletion releases every value first, while the interpreter
  still runs, so your finalizers run then.
- **Paths are absolute.** The plugin directory passed to `init` may be
  relative (`UGLY_PLUGIN_PATH=build/...`); the CLR loads assemblies by
  absolute path only.
- **Load your helper code where later code sees it.** The CLR's isolated
  load context (`load_assembly_and_get_function_pointer`) would have given
  fixture assemblies a second copy of the `Ugly` types; the default context
  (`hdt_load_assembly`) doesn't.

## Bindings

- **Link no interpreter library.** The binding is your plugin's source
  compiled again (`-DUGLY_BINDING`) as the language's native module, using
  the host's interpreter symbols. Debian's `python3` and `lua5.4` are
  statically linked: a second interpreter in the process is fatal.
  (.NET's binding is `Ugly.dll` itself; it needs no native shim.)
- **Register, then attach**: `ugly_lang_register` with the plugin compiled
  in, then `ugly_rt_attach`, so code elsewhere asking for your language
  reaches the host's interpreter instead of starting another.
- **Attach on first use, on that thread**, not at import: HAProxy loads Lua
  states on its main thread and runs them on workers.
- **Tear down on the thread that attached.** Lua uses a `__gc` sentinel,
  Python `atexit`, Perl an `END` block, Java JVMTI's VMDeath (shutdown
  hooks run on new threads). .NET's `ProcessExit` runs on another thread,
  so its teardown is an explicit `Interop.Shutdown()`.

## Performance

Measure with `tests/bench.c` (`meson test -C build --benchmark --verbose`):
C calling your language, your language natively, every pair, and threads.
[doc/benchmarks.md](benchmarks.md) has the current numbers. The first run
found these, now fixed:

- **Cache what you resolve.** .NET and Java resolved every call by
  reflection: the methods of that name, their parameters, scoring each
  overload. Cache the candidates per (type, name), and the chosen overload
  per argument signature. The signature is what the scoring looks at:
  argument types, and value classes where scoring depends on values (which
  integer types a number fits, whether a string could be a char). This cut
  calls into .NET from 358 to 145 ns.
- **Don't allocate per call.** Perl's `&{}` overload made a closure on
  every call of a foreign function (two thirds of the call's cost). Make
  such things once per proxy, without creating a cycle that keeps the
  proxy alive. Watch for APIs that copy: `GetMethods()`, `getMethods()`,
  `GetParameters()` return new arrays each time.
- **Cache traits.** Calling a class that can only be instantiated
  constructs it, which needs the callee's traits: ask once per proxy, not
  per call.
- **Don't add a lock on the call path.** The core's handle lookup takes no
  lock, so calls into separate runtimes scale with threads (it was one
  global lock, and adding threads made the total slower). A plugin-wide
  lock on every call would put that back.
- **Each crossing costs.** Every JNI call into Java costs 30 to 60 ns of VM
  transition; Java's ~440 ns per call is about five of them. Count them
  with perf (`linux-tools-generic`; JIT frames need
  `JAVA_TOOL_OPTIONS="-XX:+UnlockDiagnosticVMOptions -XX:+DumpPerfMapAtExit"`).
  Classify a value with one call into the language, not a check per type
  (Java's eleven `IsInstanceOf` were 11% of a call), and never look things
  up by name on the call path: JNI's `FindClass` walks the Java stack to
  find its caller's class loader (12% of a call, until it was cached).
- **Measure honestly.** Let JITs warm up (the benchmark's growing runs do),
  and turn debug checks off: the JVM's `-Xcheck:jni`, which the tests turn
  on, made each call into Java about 40% slower.

## Testing tips

- **valgrind** runs the whole suite (`--setup valgrind`) and fails on any
  leak. A VM whose JIT and GC are beyond memcheck goes in its own suite
  (`jvm`, `clr`), left out of the valgrind and helgrind setups.
- **Don't rely on undefined behaviour to provoke a fault.** GCC 13 folds
  `1 / x` into a comparison even at `-O0`, so `1 / 0` returned 0 and the
  `dotnet-signals` test passed on GCC 11 but not on Ubuntu 24.04. Make both
  operands `volatile`.
- **Fault tests in a child process.** Jumping out of a signal handler that
  another VM's handler called leaves that VM's state half done; let the
  child exit from the handler instead.
- **Another Ubuntu, locally**: a failure only CI's 24.04 shows can be
  reproduced in `docker run ubuntu:24.04`, building the core and your
  plugin there.
- **Pairwise finds what the suite can't**: two VMs' signal handlers, two
  runtimes' errors crossing. The JVM's `-Xcheck:jni` found perl resetting
  SIGFPE in `pairwise-java-perl5`.
