# TODO

Ideas and open work, roughly in priority order within each section. The
agreed plan lives in [doc/contract.md](doc/contract.md) ("Order of work");
this list is everything else, plus the reasons behind each item.

## Now

- [x] **QuickJS plugin** (done 2026-11-24: `plugins/js/`).
- [x] **Perl** (done 2026-11-24: `plugins/perl5/`, with a binding).
- [x] **Java** (done 2026-11-25: `plugins/java/`, with a binding).

## Embedding in real hosts: HAProxy and friends

The motivating case: an application that embeds Lua as its extension
language (HAProxy, nginx/OpenResty, Neovim, Wireshark, game engines), with
the logic you need already written in Python. `ugly.so` links no Lua of its
own, so it already uses the host's. These items stand between that and a
working setup.

- [ ] **Calling a runtime from any thread** (the real blocker). HAProxy runs
  Lua from several worker threads: one shared state behind a lock
  (`lua-load`), or one state per thread (`lua-load-per-thread`). UGLY binds
  a runtime to the thread that created it, and Python has one runtime per
  process, so every other thread gets `UGLY_ERR_WRONG_THREAD`.
  - Proposal: a capability, `UGLY_CAP_ANY_THREAD`, for plugins that
    serialize access themselves. Python does already, since every entry
    takes the GIL, which works from any thread.
  - The core already has one ctx per thread and a locked handle table. The
    work is in the runtime thread check in `rt_enter`, deferred releases,
    and the threads conformance group.
  - The contract anticipated this relaxation (§6).
- [x] **Options for runtimes created on demand** (done 2026-11-25):
  `ugly_configure` and `configure` in every binding (paths, Lua's cpath,
  Java's class path and JVM options). Perl could also take modules to load
  first; nothing else is planned.
- [ ] **Lazy Python initialization**, for hosts that fork. In HAProxy's
  master-worker mode, a `lua-load` script may run in the master before the
  workers fork, and each worker would inherit a forked interpreter (that
  would need `PyOS_AfterFork_Child`). Initializing on first use, inside the
  worker, avoids it.
- [x] **Lua 5.3 support** (done 2026-11-25): `-Dlua_version`, a binding for
  every Lua found, and a CI job with the plugin on 5.3.
- [ ] **Lua 5.1 / LuaJIT support**: OpenResty, Neovim, many games. It's a
  bigger API gap (no integers, `lua_pcall` and registry differences,
  `luaL_setfuncs`), but it reaches the most embedded Luas.
- [ ] **HAProxy proof of concept**, as a test when `haproxy` is installed: a
  small config whose Lua action calls into a Python module.
- [ ] **Document the blocking model.** A call into another language is a
  plain C call, so Lua can't yield across it. That's fine for quick work
  (lookups, scoring, validation, reusing parsing or policy code), but code
  that does I/O belongs behind the host's own asynchronous mechanisms, or
  out of process (HAProxy's SPOE). With several threads, the GIL serializes
  all the Python work.

## The contract: what v2.0 left out

All additive; none of it breaks existing plugins (contract, "Scope").

- [ ] **Keyword arguments** for foreign calls. Python's `f(x, key=...)` on a
  proxy is refused today, and so is `sorted(t, key=f)` passing through to
  another language. This needs a call-options form of `ugly_call`/`ugly_invoke`.
- [ ] **Iterators and generators**: a protocol for "next" (Python
  iterators, Lua `pairs`/closures, JS iterators), beyond today's indexing.
- [ ] **A hash protocol for non-string keys**: Lua tables keyed by tables,
  Python dicts keyed by tuples, JS `Map`.
- [ ] **Overload hints** (v1's `hint`), for statically typed languages, as
  part of the call options above. Java picks overloads by scoring how well
  the arguments convert, which settles most calls; a hint would settle the
  ties (`f(int)` against `f(long)`, `f(Object)` against `f(String)`).
- [x] **Resizing and deleting** (done 2026-11-25): `ugly_splice` and
  `ugly_delete_member`, plugin ABI 2. Perl's ties use them.
  - [ ] The other bindings' proxies could too: `del p[k]`, `p.append(x)`
    and slice assignment in Python; `table.insert` can't reach a proxy in
    Lua, but a `__newindex` past the end already appends; JS `delete` and
    `push` on proxies.
- [x] **Foreign functions in Lua's C library** (done 2026-11-25). A foreign
  function (callable, not a class) is now a real Lua function, a C closure
  holding the proxy, so `string.gsub`, `table.sort` and `coroutine.wrap`
  take it. Classes stay userdata, and a class reached as a member is the
  class too (it used to become a method closure), so
  `datetime.date.today()` works from Lua. The cost: `f.attr` on a foreign
  function. JS functions declared with `function` are constructors, so
  they stay userdata; arrow functions become Lua functions.
- [ ] **Perl sugar still missing**: stringifying a proxy with its own
  language's `str()`/`tostring` (needs a "to string" operation), `0+`,
  `<=>` and `cmp` overloads (today `fallback => 1` turns `$p + 1` into
  arithmetic on the debug string), and `each` over foreign iterators (needs
  the iterator protocol above). Instances aren't blessed into the packages
  `use Ugly` makes, so `ref($obj)` stays `Ugly::Proxy`.
- [ ] **Async/coroutines across runtimes.** It's hard, and probably needs
  each plugin to expose its scheduler.
- [ ] **Python subinterpreters** (per-interpreter GIL, 3.12+), which would
  give Python `UGLY_CAP_MULTI_RUNTIME`.
- [ ] **Cross-runtime cycles** leak by design (contract §3). Revisit only
  with a real use case.
- [ ] **Sandboxing**: out of scope unless a host needs it.

## Languages

- [x] **QuickJS** (done).
- [x] **Perl** (done; decisions 9–12 in the contract).
  - [ ] **Scalar context as a call option**, once call options exist
    (keyword arguments, above). Until then, calls run in list context and
    collapse.
  - [ ] **Booleans** use a heuristic before perl 5.36 (no `SvIsBOOL`).
    Revisit when 5.36+ is the norm.
- [x] **A JS binding** for `qjs` (done 2026-11-24): `plugins/js/binding/`.
- [ ] **Node.js**, for the npm ecosystem, which QuickJS can't run because
  it has no Node APIs (`fs`, `http`, CommonJS packages).
  - Start with the binding: Node as host, through an N-API addon. N-API is
    a stable C ABI, with references, finalizers and function calls, so
    `require("ugly")` from `node` can use Python and Lua.
  - Calls coming from other threads need `napi_threadsafe_function`. That
    overlaps the any-thread item above.
  - Embedding Node as a plugin is the harder half: libnode's C++ API, the
    event loop owned by Node, and one instance per process. Do it later, if
    ever.
- [x] **Java (the JVM)**, done 2026-11-25 through JNI (`plugins/java/`).
  What's left:
  - [x] **A Java binding** (done 2026-11-25): `ugly.Foreign` loads
    `libugly_jni.so`, whose `JNI_OnLoad` attaches; teardown at JVMTI
    VMDeath.
  - [ ] **Java threads.** A runtime is the JVM seen from one thread; other
    Java threads calling foreign values get `IllegalStateException`. "Calling a
    runtime from any thread" (above) would lift that.
  - [ ] **Panama FFM** for the helper side once JDK 22+ is the baseline.
    Creating the JVM still needs the JNI invocation API.
- [ ] **Ruby (CRuby)**: closest in shape to Python (`ruby_init` once per
  process, the GVL, objects, exceptions, blocks), so the Python plugin is
  the template. Big ecosystem, low cost.
- [ ] **WebAssembly** (wasmtime's C API, Component Model). One plugin brings
  every language that compiles to Wasm (Rust, Go, C, Zig, AssemblyScript).
  UGLY's own/borrow already follow WIT resources, so it would bridge the
  two worlds the essay compares. It's also the first plugin where memory
  isn't shared, so values are copied.
- [ ] **R** (`Rf_initEmbeddedR`): the data-science ecosystem. Everything is a
  vector (the elements protocol), and `NA` isn't null.
- [ ] **Tcl**: the HAProxy pattern again. Chip-design tools (Vivado, Quartus)
  have Tcl built in, and teams want Python from it. Everything is a string,
  a stress test for "exact or refused".
- [ ] **Guile Scheme**: built for embedding. Continuations that jump across
  foreign call frames must be detected and refused, a re-entry rule the
  contract doesn't state yet.
- [ ] **Julia** (`jl_init`): numerical work, the same once-per-process shape
  as Python.
- [ ] **Haskell**: possible, but awkward. GHC compiles, so a plugin would load
  compiled libraries (`foreign export`, `StablePtr`), not interpret.
  Dynamic calls need `Data.Dynamic` or the GHC API. What it would test: the
  essay's "types flow from the static side", where `want` and hints carry
  real weight.
- [ ] **Bindings for compiled languages**, with no plugin: a safe Rust crate,
  a C++ header, and Go and Zig wrappers over `ugly.h`. They're cheap, and
  bring every embedded language to programs in those languages. (Go and
  Erlang can't sensibly be embedded as plugins.)

## Engineering

- [x] **CI** (done 2026-11-25, `.github/workflows/ci.yml`): all languages
  on Ubuntu 22.04 and 24.04 with `-Dwerror=true`, `meson test`, valgrind
  and helgrind (threads group); one build without each language, and one
  with none.
  - [x] A Lua version axis: the plugin on 5.3 is a job, and both bindings
    are tested in the others. LuaJIT joins it when it's supported.
- [ ] **Attach tests in C.** Attach mode is tested only through the binding
  scripts. A C test that creates its own Lua state and attaches to it would
  bring attach into the conformance suite proper.
- [ ] **Install layout.** `meson install` warns that the Python extension's
  destination may not be on `sys.path` under `/usr/local`. Set
  `python.platlibdir` in the defaults, or document `-Dpython.install_env`.
- [ ] **Plugin helper directory.** `init(ctx, dir)` passes the plugin's
  directory, but no plugin uses it yet. Use it for language-side helper
  files, or drop it.
- [ ] **GitHub setting**: "Automatically delete head branches" is off, so
  merged branches are deleted by hand. That's the author's call.

## Docs

- [ ] **Design essay epilogue.** `doc/design-essay.md` ends with "what a v2
  would have to decide first". v2 now exists, so a short epilogue could say
  which of those decisions held, and what the implementation changed (the
  depth limit, receivers across languages, rule 7, static interpreters and
  bindings).
