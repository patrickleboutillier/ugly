# TODO

Open work, roughly in priority order within each section, with the reasons
behind each item; what's done is listed at the end. The contract,
[doc/contract.md](doc/contract.md), records every decision taken.

## Review findings (2026-11-27)

A bug hunt over the core and the eight plugins, after 2.7.0. Each was
confirmed by a repro unless marked *(by reading)*. Line numbers are 2.7.0's.
The crashes, the leaks, the wrong results and the lost errors are fixed (under "Done").

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

### Wrong results: what's left

The thirteen are fixed (under "Done"). Seen while fixing them:

- [ ] **Perl: `isa`, `can`, `DOES` and `VERSION`** on a proxy are Perl's
  own, not the foreign value's (Perl code calls them on any object).

### Smaller

- [ ] **Python binding imported first on another thread:** teardown
  (`atexit`, on the main thread) does nothing.
- [ ] **Lua: an `ugly.error` kept after it escaped once is empty.**
- [ ] **Lua binding: a `__gc` after the sentinel re-attaches the state**,
  and that attach is never torn down.
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

## Design questions (2026-11-27)

From a read of the design after the review, for consideration: none is
decided. Unlike the next section, the first two would change what existing
plugins do.

- [ ] **`MEMBERS` holds both attributes and entries.** A Python dict's
  members are its keys, a Java `Map`'s come after its fields and bean
  getters, a Perl hash's before its object's methods (contract §10). So a
  key named like a method (`keys`, `get`) is ambiguous, and each plugin
  settles it its own way. Truffle keeps hash entries apart from members.
  The hash protocol (below) is listed for non-string keys, but it is also
  the other half of this split: once it exists, a string key of a dict has
  two routes. Decide which is canonical before writing it, and whether
  `get_member` on a dict keeps reaching its keys.
  *Recommendation (2026-11-27), parked by the author:* split them. A new
  trait `ENTRIES` with `get_entry`, `set_entry`, `delete_entry` and
  iteration over keys, the keys any `ugly_val`: this is the hash protocol,
  one piece of work, not two. `MEMBERS` becomes what the language's dot
  reaches (attributes, fields, methods), so `get_member` on a dict stops
  reaching its keys. A Python dict, a Java `Map`, a C# dictionary, a JS
  `Map`, a Perl hash and a Haskell `Map` report `ENTRIES` for their keys;
  a Lua table reports both (its string keys are both). Bindings whose
  syntax tells the two apart use it (`p.k` and `p["k"]` in Python,
  `$p->m` and `$p->{k}` in Perl); those where it's one thing (Lua, JS) try
  the member, then the entry, with an explicit `ugly.entry(p, k)` for an
  entry a method shadows. The fallback is the binding's: the core stays
  exact or refused. Keys compare by the home language's own rule. About
  the size of the iteration work: plugin ABI 6, core and mock first, then
  a language per pull request.
- [ ] **Deferred release as the core's default.** Rule: `release` "must be
  safe to re-enter, or defer the work itself". JS got it wrong (a
  destructor re-entering inside QuickJS's collection), and the any-thread
  languages are always queued already. The core could queue every release
  that arrives while the home runtime is inside a call, and drain when the
  outermost call returns, as it enforces borrows instead of documenting
  them. Immediate release would be a capability: Perl needs it (decision
  12, `DESTROY` when the last reference goes).
- [ ] **No fourth thread mode before the races are closed.** Three exist
  (the owner's thread, `UGLY_CAP_ANY_THREAD`, host-serialized), the last on
  a promise the core can't check, and both of the core's open races are
  here (`ugly_rt_get`, fixed since; deleting a shared runtime, still
  open: "Review findings"). Worth
  a pass over §6 as one state machine once they're fixed, rather than as
  three relaxations.
- [ ] **The blocking model is a harder limit than cycles.** Cycles leak, a
  bounded cost. A blocking call can't be worked around by a host that owns
  an event loop: a Node binding (below) stalls Node for as long as Python
  runs. Settle what the contract says about it (refuse, a thread per
  call, or the async item below) before the Node binding, not after.
- [ ] **The plugin ABI when plugins live out of tree.** An exact match and
  twin slots (`call` and `call_opts`) are right while all eight are built
  with the core. A plugin released on its own (Wasm, written in Rust)
  breaks at every bump: four in the first four days. Options: a `size` first in
  `ugly_plugin`, as `ugly_opts` has, with slots past it read as NULL; or
  keep the exact match and say that plugins ship with the core.

## The contract

All additive: none of it breaks existing plugins (contract, "Scope").

- [ ] **A hash protocol for non-string keys**: Lua tables keyed by tables,
  Python dicts keyed by tuples, JS `Map`. See "Design questions": it also
  decides where a dict's string keys belong.
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
- **Review fixes, wrong results** (2026-11-27): threads asking together
  for an any-thread language's first runtime all share the one made;
  Python: a class held as a member is the class, `bool()` of a value
  without a length, traits not cached from a failed describe; Perl: hash
  keys beyond ASCII, `!=` and `ne`, the XS functions out of `Ugly::Proxy`'s
  way (`Ugly::Core`); Java: narrowing checked for fields, elements and
  results, a hint never left for the next call; C#: an enumerable that is
  its own enumerator can be iterated again, a wide integer's DOUBLE;
  Haskell: identity keyed on the type too; JS: no symbol-keyed stores.
  Then, with no users to break: a JS class is INSTANTIABLE and not
  CALLABLE, so calling it from Python or Lua constructs it; Lua keeps a
  callable class member's owner, as Python does (`this` for a JS function
  member); Python's `bool()` asks for a first element, not a length.
- **Review fixes, lost errors** (2026-11-27): an error's language, type
  and message are valid UTF-8 whatever they were made from (the core drops
  a character cut by the end of the text and puts U+FFFD for other bytes
  that aren't UTF-8), so Python no longer raises `UnicodeDecodeError` in
  place of the error; a Perl `die` with Latin-1 text keeps its characters.
- **First-hour papercuts** (2026-11-27): Java and C# rank `double` first
  for an integer (`Math.signum(2)`, `Math.Round(5)` were "ambiguous");
  Python's buffered output is flushed when its runtime goes and at exit
  (lost when piped); error messages cross whole (Lua, Python, Perl and JS
  cut them at 511 bytes); a Python exception that comes home keeps its
  traceback; `Map()` and `Set()` construct when called from another
  language; Lua's `ugly.tostring(f)` for a foreign function, and
  `p[1.0]` is `p[1]`.
- **Resizing and deleting from Python and JS** (2026-11-27): a foreign
  sequence takes `del p[i]`, slices (read, assigned, deleted), negative
  indexes and a list's `append`, `extend`, `insert`, `pop` and `clear` in
  Python; `push`, `pop`, `shift`, `unshift` and `splice` in JS; and
  `del p.k`, `del p["k"]`, `delete p.k` remove a member. A member of that
  name on the foreign value comes first.
- **Docs:** the README's tradeoffs (isolation, the blocking model, cycles,
  local builds); Apache-2.0 after 2.4.0 (GPL v3 before).
