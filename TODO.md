# TODO

Open work, roughly in priority order within each section, with the reasons
behind each item; what's done is listed at the end. The contract,
[doc/contract.md](doc/contract.md), records every decision taken.

## Review findings (2026-11-27)

A bug hunt over the core and the eight plugins, after 2.7.0. Each was
confirmed by a repro unless marked *(by reading)*. Line numbers are 2.7.0's.
The crashes and the leaks are fixed (under "Done").

### Crashes: what's left

The eight crashes are fixed (under "Done"). Seen while fixing them, by
reading only:

- [ ] **Haskell: only `call` and `invoke` check the stack** (`STACK_CHECK`);
  `get_member`, `new_instance`, `iter` and `next` enter Haskell unchecked.
- [ ] **Perl: `&ST(1)` handed to `take_kw`/`xs_args`** while converting an
  argument can run a tied `FETCH` that moves perl's stack.
- [ ] **Perl: what an entry had allocated in C is lost when it croaks**
  (the guard catches the croak; the entry's `malloc`s and owned handles at
  that point leak).
- [ ] **JS: a finalizer after the binding's teardown releases on the
  spot.** Nothing of ours is left to re-enter then, but another language's
  destructor still runs inside QuickJS's collection.

### Wrong results

- [ ] **Core: `ugly_rt_get` isn't atomic for any-thread languages**
  (`rt.c`): between `lang_share_rt` and `ugly_rt_new`, threads making their
  first call together get "python supports one runtime per process" (7 of
  8 threads, in 1 run of 30).
- [ ] **Python: a member that is a class becomes a bound method**
  (`proxy_getattro` tests `CALLABLE` only; Lua also requires not
  `INSTANTIABLE`): `m.C.s` and `m.C(1)` fail for a JS class.
- [ ] **Python: `bool(proxy)` raises `TypeError`** for a value without a
  length (`if callback:` fails), and is `False` for a Lua table with only
  fields. An `nb_bool`.
- [ ] **Python: `proxy_traits` caches 0 forever** after one failed
  `ugly_describe` (a call from the wrong thread). Cache only on `UGLY_OK`.
- [ ] **Perl: non-ASCII hash keys** are unreachable and stored as bytes:
  `hv_fetch`/`hv_store`/`hv_delete` get a positive `klen`, and
  `hv_iterkey` ignores `HeKUTF8`.
- [ ] **Perl: `!=` and `ne` on proxies are always false** (`helper.pm`
  overloads `==` and `eq` only, with `fallback`).
- [ ] **Perl: foreign methods named like `Ugly::Proxy`'s own subs**
  (`_get`, `isa`, `_release`...) never reach `AUTOLOAD`; `$o->_release`
  releases the handle. Move the XS functions to another package.
- [ ] **Java: `coerce()` narrows without a range check** where `score()`
  didn't run first: `setMember`, `setIndex` on arrays, a foreign
  function's result for an interface (`2**40+7` into an `int` field
  stores 7; `'xyz'` into a `char` stores `'x'`).
- [ ] **Java: a hint on a callable that isn't overloaded is dropped**
  (`Bridge.call`'s functional-interface path never consumes `HINT`), and
  the next overload resolution on that thread, a nested call, takes it.
- [ ] **C#: an object both `IEnumerable` and `IEnumerator`** (`yield`,
  LINQ) is single-shot once iterated: the second pass is empty, and
  releasing its iterator doesn't `Dispose()` while the enumerable is held.
  Hand out a wrapper enumerator when `GetEnumerator()` returns the object
  itself.
- [ ] **C#: `want = DOUBLE` on a wide integer that doesn't fit int64**
  (`Bridge.cs`, `Wide` ignoring `fits`): `ulong.MaxValue` gives -1,
  `Int128.MaxValue` 0.
- [ ] **Haskell: identity collides for values sharing a heap object**
  (`newRef`: the `StableName` alone): a newtype and its contents, empty
  `Map`s at different types. One comes back with the other's box. Key on
  the type too.
- [ ] **JS: symbol-keyed assignment on a proxy** writes a string member
  (`d[Symbol.iterator] = 2` makes a key "Symbol.iterator").

### Lost errors

- [ ] **Messages cut mid-character, or not UTF-8.** Lua, Python, Perl and
  JS cut messages at 511 bytes with `snprintf`, and Lua and Perl pass
  bytes that aren't UTF-8. Python then raises `UnicodeDecodeError` instead
  of `ugly.Error` (`raise_ugly`): type, language and payload are lost.
  Sanitize once in the core (`error_new`), and decode leniently in Python.
  (Haskell decodes leniently since the crash fixes.)

### Smaller

- [ ] **Python embedded: buffered stdout is lost at exit** when piped
  (Python is never finalized). Flush in `py_rt_delete`, or document it.
- [ ] **Python: a round-tripped exception loses its inner traceback**
  (`py_failure` drops `tb`).
- [ ] **Python binding imported first on another thread:** teardown
  (`atexit`, on the main thread) does nothing.
- [ ] **Lua: `lst[1.0]` is a member lookup** (`lua_isinteger`); plain Lua
  normalizes it.
- [ ] **Lua: `tostring` of a foreign function** is the closure's address;
  the guide says the foreign stringification.
- [ ] **Lua: an `ugly.error` kept after it escaped once is empty.**
- [ ] **Lua binding: a `__gc` after the sentinel re-attaches the state**,
  and that attach is never torn down.
- [ ] **Java and C#: an integer ties between `float` and `double`**
  (`Math.signum(2)`, `Math.Round(5)`: "ambiguous call"). Rank `double`
  first.
- [ ] **Java: lax UTF-8 in `new_string`** (overlongs, encoded surrogates,
  beyond U+10FFFF); `c_string` cuts at a NUL; `getMember(obj, "")` raises;
  attached threads are never detached; `jv_next` drops the rest of a batch
  after a failed conversion *(by reading)*.
- [ ] **C#** *(by reading)*: `Conv.Export` releases again after a failed
  `ugly_export` (the core already did); `Errors.Fail` can throw out of an
  `[UnmanagedCallersOnly]` method on a stale payload; multi-dimensional
  arrays report ELEMENTS.
- [ ] **Java and C#: a VM thread using a `Foreign` after the runtime was
  deleted** reaches `ugly_fail(ctx, NULL, ...)`. *(By reading.)*
- [ ] **Haskell binding:** `withUgly` from two threads at once races
  (`binding.c`'s `b_depth`, `b_ctx`). *(By reading.)*
- [ ] **Perl:** a string once used as a number goes out as a number (IOK
  before POK): a design call, or a pitfall for the guide.
- [ ] **JS** *(by reading)*: a throwing `done` getter reads as "finished";
  `ugly.opts({context: 5})` is silently ignored.
- [ ] **Mock** *(by reading)*: `want = STRING` on BYTES isn't validated as
  UTF-8.
- [ ] **Core:** the "c" language's INT to DOUBLE (`host.c`, `load`) says
  lossless and doesn't check ±2^53; `ugly_ctx_delete` leaves
  `ugly_ctx_thread`'s pthread key set; deleting a shared any-thread runtime
  can race with a thread between `h_lookup` and `rt_enter` (teardown only).

## Embedding in real hosts

The motivating case: an application with Lua built in (HAProxy,
nginx/OpenResty, Neovim, Wireshark, game engines), with the logic you need
already written in Python. HAProxy works, in both of its Lua modes
([examples/haproxy/](examples/haproxy/)).

- [ ] **Lua 5.1 / LuaJIT**: OpenResty, Neovim, many games. A bigger API gap
  than 5.3 was (no integers, `lua_pcall` and registry differences,
  `luaL_setfuncs`), but it reaches the most embedded Luas. One Lua per
  process: it would be another `-Dlua_version`, and another binding.

## The contract

All additive: none of it breaks existing plugins (contract, "Scope").

- [ ] **A hash protocol for non-string keys**: Lua tables keyed by tables,
  Python dicts keyed by tuples, JS `Map`.
- [ ] **Resizing and deleting in the other bindings.** `ugly_splice` and
  `ugly_delete_member` exist, and Perl's ties use them. Python proxies
  could too (`del p[k]`, `p.append(x)`, slice assignment), and so could JS
  (`delete`, `push`). In Lua, a `__newindex` past the end already appends.
- [ ] **Async and coroutines across runtimes.** Hard; probably needs each
  plugin to expose its scheduler.
- [ ] **Python subinterpreters** (a GIL per interpreter, 3.12+), which would
  give Python `UGLY_CAP_MULTI_RUNTIME`.
- [ ] **Cross-runtime cycles** leak by design (contract §3). Revisit only
  with a real use case.
- [ ] **Sandboxing**: out of scope unless a host needs it.

## Languages

- [ ] **Perl's remaining sugar**: `0+`, `<=>` and `cmp` overloads (today
  `fallback => 1` turns `$p + 1` into arithmetic on the foreign value's
  string), and instances blessed into the packages `use Ugly` makes (`ref($obj)` is
  `Ugly::Proxy`). Also `ugly_configure("perl5", "modules", ...)`, to load
  modules first.
- [ ] **Perl booleans** use a heuristic before perl 5.36 (no `SvIsBOOL`).
  Revisit when 5.36+ is the norm.
- [ ] **Ruby (CRuby)**: closest in shape to Python (`ruby_init` once per
  process, the GVL, objects, exceptions, blocks), so the Python plugin is
  the template. Big ecosystem, low cost.
- [ ] **Node.js**, for the npm ecosystem, which QuickJS can't run (no `fs`,
  `http`, CommonJS packages).
  - Start with the binding: Node as host, through an N-API addon, a stable
    C ABI with references, finalizers and calls. `require("ugly")` from
    `node` would then use Python and Lua.
  - Calls from other threads need `napi_threadsafe_function`.
  - Embedding Node as a plugin is the harder half: libnode's C++ API, an
    event loop Node owns, one instance per process. Later, if ever.
- [ ] **WebAssembly** (wasmtime's C API, Component Model). One plugin brings
  every language that compiles to Wasm (Rust, Go, C, Zig, AssemblyScript).
  UGLY's own/borrow already follow WIT resources, so it would bridge the
  two worlds the essay compares. The first plugin where memory isn't
  shared, so values are copied.
  - Components, not core modules: a core module exports only numbers and
    a linear memory. WIT's strings, lists, records, options and results
    map onto `ugly_val` and core lists and maps.
  - A guest has no object model, so it sees foreign values through a WIT
    package for UGLY itself: a `foreign` resource with get, set, call and
    invoke, imported by the guest. Its callables, and its own resources,
    are what the other languages see.
  - The one runtime that is isolated: a trap fails the call, not the
    process, and WASI capabilities are granted per runtime (`ugly_configure`).
  - Check how much of wasmtime's component API its C API covers; if too
    little, write the plugin in Rust against the C vtable.
- [ ] **NuGet packages for .NET modules.** A .NET program has its packages
  already; C# reached from another language finds assemblies on the module
  path only. Resolving a package's dependencies (`*.deps.json`) would let
  `ugly.module("dotnet", ...)` use NuGet libraries directly.
- [ ] **.NET's lazy signal handlers.** Starting the CLR leaves the host's
  signals alone, but C# code that uses `Console` or `Process` makes .NET's
  System.Native install terminal and child-process handlers (SIGCHLD,
  SIGCONT, SIGWINCH...) at that point. Test it, and decide whether the
  plugin should do anything (it can't take them back without breaking
  `Process`).
- [ ] **Java through Panama FFM** for the helper side, once JDK 22+ is the
  baseline. Creating the JVM still needs the JNI invocation API.
- [ ] **R** (`Rf_initEmbeddedR`): the data-science ecosystem. Everything is a
  vector (the elements protocol), and `NA` isn't null.
- [ ] **Tcl**: the HAProxy pattern again. Chip-design tools (Vivado, Quartus)
  have Tcl built in, and teams want Python from it. Everything is a string,
  a stress test for "exact or refused".
- [ ] **Guile Scheme**: built for embedding. Continuations that jump across
  foreign frames must be detected and refused, a re-entry rule the contract
  doesn't state yet.
- [ ] **Julia** (`jl_init`): numerical work, the same once-per-process shape
  as Python.
- [ ] **Bindings for compiled languages**, with no plugin: a safe Rust crate,
  a C++ header, Go and Zig wrappers over `ugly.h`. Cheap, and they bring
  every embedded language to programs in those languages.

## Engineering

- [ ] **Java's call cost.** About 440 ns per call into Java (from 1 µs:
  doc/benchmarks.md), against 60 to 160 ns for the other languages. Profiled
  with perf: what's left is about five JNI calls into Java per call
  (boxing, the call, classifying and unboxing the result, the argument
  array), 30 to 60 ns each. Next: one Java entry point per call that boxes
  and unboxes itself (primitive arguments in a long[]/double[]), or
  Panama upcalls once JDK 22+ is the baseline.
- [ ] **Python called from many threads.** Every call takes the GIL, which
  changes hands on every call when several threads call at once (about
  8 µs per call with 4 threads). Nothing UGLY can do short of a
  free-threaded CPython (3.13t), but a binding could offer a batch call
  (many calls under one GIL hold).
- [ ] **Packages**: `.deb` per Ubuntu LTS (binaries are valid per distro
  release), then a LuaRocks rock or a Python wheel per binding, if anyone
  wants them. Releases are source tarballs today.
- [ ] **Install layout.** `meson install` warns that the Python extension's
  destination may not be on `sys.path` under `/usr/local` (set
  `python.platlibdir`, or document `-Dpython.install_env`). The Lua
  bindings go to Lua's own C-module directory from pkg-config
  (`/usr/lib/<triplet>/lua/<version>`), outside `--prefix`.
- [ ] **Attach tests in C.** Attach mode is tested through the binding
  scripts only; a C test that creates a Lua state and attaches to it would
  bring it into the conformance suite.
- [ ] **Plugin helper directory.** `init(ctx, dir)` passes the plugin's
  directory, and no plugin uses it. Use it for language-side helper files,
  or drop it.

## Docs

- [ ] **Design essay epilogue.** `doc/design-essay.md` ends with "what a v2
  would have to decide first". A short epilogue could say which decisions
  held, and what building it changed (the depth limit, receivers across
  languages, rule 7, static interpreters and bindings, threads, call
  options).

## Done

Released as 2.2.0 (the first release) through 2.7.0; details in the
contract and in the pull requests.

- **Languages:** QuickJS, Perl 5 (with namespace import and complete ties),
  Java through JNI (with a binding, and any thread), Lua 5.3 beside 5.4,
  C# on the CLR through `hostfxr` (any thread, keyword arguments as named
  arguments, hints), and its binding (`Ugly.dll` in any .NET program,
  through `dynamic`).
- **Haskell** (GHC, 2026-11-26): the language that tested the contract's
  assumptions most. Modules are compiled export tables Template Haskell
  generates, in module libraries on a path; polymorphic functions are
  expanded per instance and chosen at the call (a hint is a type
  application, `want` picks by result); foreign values and callbacks in
  Haskell; the conformance suite and pairwise; music-diatonic from Python
  and Lua; the binding (`Ugly.withUgly`, a Haskell program as host). Guide:
  [doc/languages/haskell.md](doc/languages/haskell.md).
- **Hosts:** HAProxy, in `lua-load-per-thread` and `lua-load` on several
  threads, and in master-worker mode with Python started in the master;
  options for runtimes created on demand (`ugly_configure`). Forking after
  Python started: the plugin brackets every fork like `os.fork()`, and the
  core holds its locks across it.
- **Threads:** `UGLY_CAP_ANY_THREAD` (Python, Java), runtimes shared between
  threads, `ugly_ctx_thread`, and host-serialized runtimes (contract §6).
- **Contract:** call options (keyword arguments, Perl's context, Java's
  hints; §11), `ugly_splice` and `ugly_delete_member` (and
  `UGLY_TRAIT_FIXED_SIZE`, so splicing an infinite list is refused rather
  than hanging), foreign functions as
  real Lua functions, classes that construct when called.
- **Iteration and stringification** (contract §2, decisions 18 to 25;
  libugly 2.7, plugin ABI 5; 2026-11-27): `ugly_iter`, `ugly_next` (batches
  that never run ahead) and `ugly_to_string`, in the core, core lists, the
  mock and all eight languages, each binding hooking proxies into its own
  loop (Perl: `Ugly::iter`, objects with a `next` method), with the
  `iterators` group and pairwise checks for every pair.
- **Engineering:** CI (two Ubuntu releases, valgrind, helgrind, a build
  without each language, Lua 5.3), source releases from a tag. Benchmarks
  (`tests/bench.c`, doc/benchmarks.md), which found the core's lock
  serializing every call (now a lock-free lookup), and uncached reflection
  in .NET and Java, and a closure per call in Perl.
- **Review fixes** (2026-11-27, PR #74): Lua's stack is asked for room
  before a call's arguments are pushed (a heap overflow past the free
  slots); Python reads `proxy[key] = v`'s key before releasing the GIL
  (`proxy[-1] = v` was a segfault); libugly exports only `ugly_*`
  (`src/ugly.map`), so a host can't interpose its internals.
- **Review fixes, leaks** (2026-11-27): the any-thread languages' bindings
  run the releases queued for their runtime wherever their own code uses
  `ugly` (a Python, Java, C# or Haskell program that only called out never
  freed what it passed); Java's `Foreign` cache drops collected entries;
  every entry into Perl is a scope for temporaries (a caller that isn't
  Perl leaked an RV per call of a package's sub, and kept arrays it had
  iterated); Lua's `proxy[key] = v` reads the key before converting the
  value.
- **Review fixes, the other crashes** (2026-11-27): JS queues what a
  QuickJS finalizer has to release and runs it where JS may run (a
  destructor re-entering JS inside the collection was a segfault), and a
  job run after a failed call no longer erases its error; every entry into
  Perl runs under `eval` (a croak in perl's API ended the process, or
  jumped over the core's frames), `keys %$proxy` re-reads perl's stack
  pointer, and ithreads don't clone proxies; Haskell's calls out run on one
  OS thread from start to end (a `forkIO` thread used another thread's
  ctx), `raise` can't throw, the stack check works on small stacks, and
  `set_index` on an infinite list is refused before its length is asked.
- **Docs:** the README's tradeoffs (isolation, the blocking model, cycles,
  local builds); Apache-2.0 after 2.4.0 (GPL v3 before).
