# TODO

Open work, roughly in priority order within each section, with the reasons
behind each item; what's done is listed at the end. The contract,
[doc/contract.md](doc/contract.md), records every decision taken.

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

- [ ] **Iterators and generators** (contract §2, decisions 18 to 25): the
  core (`ugly_iter`, `ugly_next`; libugly 2.7, plugin ABI 5), core lists,
  the mock, the `iterators` group, Python, Lua and Haskell (with their
  bindings' syntax, and the pairwise checks) are done. Next, each remaining
  plugin as producer and consumer, with its binding's syntax, guide and §10
  row: JS, C#, Java, Perl (and Perl's `each`).
- [ ] **A hash protocol for non-string keys**: Lua tables keyed by tables,
  Python dicts keyed by tuples, JS `Map`.
- [ ] **Resizing and deleting in the other bindings.** `ugly_splice` and
  `ugly_delete_member` exist, and Perl's ties use them. Python proxies
  could too (`del p[k]`, `p.append(x)`, slice assignment), and so could JS
  (`delete`, `push`). In Lua, a `__newindex` past the end already appends.
- [ ] **"To string"** (`ugly_to_string`, contract §2): in the core, the
  mock, Python, Lua and Haskell; each remaining plugin implements it, and its
  proxies use it, with iteration.
- [ ] **Async and coroutines across runtimes.** Hard; probably needs each
  plugin to expose its scheduler.
- [ ] **Python subinterpreters** (a GIL per interpreter, 3.12+), which would
  give Python `UGLY_CAP_MULTI_RUNTIME`.
- [ ] **Cross-runtime cycles** leak by design (contract §3). Revisit only
  with a real use case.
- [ ] **Sandboxing**: out of scope unless a host needs it.

## Languages

- [ ] **Perl's remaining sugar**: `0+`, `<=>` and `cmp` overloads (today
  `fallback => 1` turns `$p + 1` into arithmetic on the debug string),
  `each` over foreign iterators (needs the iterator protocol), and
  instances blessed into the packages `use Ugly` makes (`ref($obj)` is
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

Released as 2.2.0 (the first release) through 2.6.0; details in the
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
- **Engineering:** CI (two Ubuntu releases, valgrind, helgrind, a build
  without each language, Lua 5.3), source releases from a tag. Benchmarks
  (`tests/bench.c`, doc/benchmarks.md), which found the core's lock
  serializing every call (now a lock-free lookup), and uncached reflection
  in .NET and Java, and a closure per call in Perl.
- **Docs:** the README's tradeoffs (isolation, the blocking model, cycles,
  local builds); Apache-2.0 after 2.4.0 (GPL v3 before).
