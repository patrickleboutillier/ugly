# CLAUDE.md

## Project: UGLY (Ultimate Glue LaYer)

A C hub for in-process, multi-language interop. It embeds the real interpreters and routes every cross-language call through a neutral C core. It was first written in 2013–2014 by the author of Perl's **Inline::Java**, as an attempt to generalize that Perl↔Java bridge to N languages: one adapter per language instead of N² pairwise bridges. It was revived in 2026 around a new contract, `doc/contract.md`.

Treat the author as a domain expert on language interop. Discuss design tradeoffs; don't explain JNI, callbacks or cross-runtime lifetimes.

## Architecture

- **`doc/contract.md`** is the spec. It was approved on 2026-11-24 (PR #1), and its "Decisions" and "Clarifications" sections record everything settled since. Every rule in it should be a test in `tests/`.
- **`include/ugly.h`** is the public API, and **`include/ugly_plugin.h`** is the plugin contract (the plugin rules plus the core services). The plugin vtable is written by hand; nothing is generated.
- **`src/`** is the core (`libugly` 2.0.0):
  - `handles.c`: the handle table. A handle is a slot index plus a generation. It also holds borrow slots, the (home, identity) index and the per-runtime queues of deferred releases, all under one lock. Plugin code never runs under that lock.
  - `ctx.c`: one ctx per thread (`_Thread_local current`) and errors.
  - `rt.c`: runtimes and frames. `rt_enter`/`rt_leave` check the thread, enforce `UGLY_MAX_DEPTH`, drain deferred releases and revoke borrows. `after_call` normalizes plugin results.
  - `protocol.c`: the operations.
  - `lang.c`: plugin loading through `UGLY_PLUGIN_PATH`, with `RTLD_GLOBAL` and counted loads.
  - `host.c`: the built-in "c" language, which is the home of C callables and core lists and maps. Each ctx has one runtime of it.
- **`plugins/<lang>/`** holds the plugins, built as `ugly_<lang>.so` and exporting `ugly_plugin_<lang>()`. Each one's `meson.build` appends to `plugins`, `plugin_dirs` and `test_plugins`, and is optional (`-D<lang>=`).
  - **`mock/`** is a small language in C (lenient, Perl-like conversions) that implements every plugin rule and every fixture.
  - **`lua/`** is Lua 5.4.
    - Every operation runs under `lua_pcall` (`op_body`), except `call`, which pcalls the target directly so a call costs one of Lua's roughly 200 nested C calls.
    - Proxies are `ugly.proxy` userdata, cached per owned handle in a weak table. Foreign errors travel as `ugly.error` userdata, with `e.payload`, and are restored unchanged when they escape.
    - Elements are 0-based in the protocol and 1-based in Lua.
    - `invoke` passes `self` only to methods found through the metatable, not to raw fields.
  - **`python/`** is CPython 3.10+.
    - Python is initialized once at plugin load with `Py_InitializeEx(0)`, the GIL is released, and Python is never finalized. So there's no RESTARTABLE and no MULTI_RUNTIME, and a runtime is a globals dict.
    - Every entry does `PyGILState_Ensure`/`Release`.
    - The embedded `ugly` module holds `ugly.Proxy` (cached per handle via weakrefs), `ugly.Error` (`.type`, `.message`, `.language`, `.payload`, and an `_err` capsule that's restored unchanged when it escapes) and `ugly.rethrow`. It is the seed of the Python binding.
    - Conversions are Python's strict ones.
  - **`js/`** is QuickJS, the language `"js"`, from the pinned wrap `subprojects/quickjs.wrap` (sha256-checked; `subprojects/packagefiles/quickjs/meson.build` builds the engine sources as a static PIC library, with `-w`).
    - One JSRuntime plus JSContext per ugly runtime. Refs are boxed JSValues; identity is the object pointer.
    - Proxies are two exotic classes (`proxy_class`, and `proxy_fn_class` with `call`), cached per owned handle in a C hash table that the finalizer clears.
    - Numbers: integral within ±2^53 → INT, else DOUBLE; larger INTs go in as `BigInt`.
    - Errors are `error_class` objects inheriting `Error.prototype`.
    - Modules are ES modules from `UGLY_JS_PATH`, plus the built-in C module `"ugly"` (`module`, `eval`, `isError`, `isProxy`, `gc`).
    - Jobs run when the outermost call returns (`enter`/`leave`), but not when attached, since the host owns the event loop. `JS_FreeRuntime` asserts that nothing is still referenced.
    - Runtimes are found through a registry (`runtimes`), not the context opaque, because an attached context belongs to `qjs`. Refs are `jsbox` entries linked per runtime.
    - The binding (`binding/`, `js_init_module`, loaded by `qjs` as `import "…/ugly.so"`) tears down from a global sentinel object, whose `gc_mark` declares every box to QuickJS's GC. Otherwise the external references would keep `qjs`'s context alive past its `JS_FreeRuntime` assertion. The subproject also builds `qjs` (`qjsc`, `repl.c`, `-rdynamic`) for the binding test.
    - Editing `subprojects/packagefiles/quickjs/` needs `rm -rf subprojects/quickjs-2026-06-04` (git-ignored) to take effect: Meson copies `packagefiles/` only at extraction.
  - **`perl5/`** is Perl 5 (5.34 here, MULTIPLICITY), written in C against the perl API with no XS generator.
    - `ENTER_PERL`/`LEAVE_PERL` set and restore `PERL_GET_CONTEXT` on every entry.
    - Refs are SVs. Foreign values are `Ugly::Proxy`: a blessed scalar ref holding the handle, cached per handle. Its Perl side (overloads, `AUTOLOAD`, tied hash and array) is the `helper_pm` string, defined in every interpreter; the XS functions are registered with `newXS`.
    - Modules and classes are `Ugly::Package` objects (a scalar ref holding the package name). Their identity is the stash, members are subs and nested packages, and `new` instantiates.
    - Calls run in list context and collapse (`call_perl`). Never `PUSHMARK` before `eval_sv`: it pushes its own mark.
    - The binding (`binding/`: `lib/Ugly.pm`, `auto/Ugly/Ugly.so`, `boot_Ugly`) attaches to the running perl. `END { Ugly::_teardown() }` tears down. Its test sets `PERL_DESTRUCT_LEVEL=2`, so the host frees everything and valgrind measures only UGLY.
  - Proxies in all four languages turn a callable member into `ugly_invoke`, so `obj.m(x)` uses the foreign language's receiver rules.
  - **Bindings** (`plugins/<lang>/binding/`) compile the plugin's own source again with `-DUGLY_BINDING`, as the native module `ugly`: `luaopen_ugly`, and `PyInit_ugly` via `pyinst.extension_module`. They link **no interpreter library**, because Debian's `python3` and `lua5.4` are statically linked and a second interpreter in the process would be fatal.
    - On import, a binding calls `ugly_lang_register` (no dlopen) and `ugly_rt_attach` on the host interpreter. Teardown is a `__gc` sentinel in Lua and `atexit` in Python.
    - Every runtime, embedded or attached, preloads the same `ugly` module: `ugly.module(lang, name)` and `ugly.eval(lang, code)`, which use `ugly_rt_get`, plus `is_error`/`is_proxy` in Lua and `Error`/`Proxy`/`rethrow` in Python.
    - The Lua plugin keeps its runtime in the registry (`ugly.rt`), not `LUA_EXTRASPACE`, and converts on the `lua_State` it was given, so it works inside coroutines.
  - Rule 7 is complete: an error whose payload comes home is raised as the original object (`throw_ugly_error`, `raise_ugly`).
- **`tests/`** is the conformance suite: 8 C programs plus `conformance.h` (TAP and fixture), and `pairwise.c`.
  - Meson registers each program once per plugin in `test_plugins` (suites `conformance` and `conformance-<plugin>`, env `UGLY_TEST_PLUGIN`), and `pairwise.c` once per ordered pair of different plugins (suite `pairwise`, env `UGLY_TEST_PEER`).
  - Fixtures are each plugin's `conformance` module, in `tests/lua/` and `tests/python/` (the mock's are built in). The spec is in contract §9.
  - Expectations that depend on the language are "exact or refused". Tests that need a capability a plugin lacks report a SKIP; they never drop checks silently.
- **`examples/`**: programs built and run as tests by exit status, which README quotes. `python_from_lua.lua` is a plain Lua script using Python's standard library through the binding (README's lead example). `polyglot.c` is a C host using `ugly_rt_get`.
- **Binding tests** (suite `bindings`): `tests/python/test_binding.py` runs under the `python3` that Meson found, and `tests/lua/test_binding.lua` under `lua5.4`. Both are TAP, and the Lua one ends with `os.exit(code, true)`, so the state closes and the binding tears down.
- Rules that are easy to get wrong:
  - Arguments are borrowed and revoked on return, so a plugin must `ugly_dup` what it keeps. Results are owned.
  - `ugly_release`, `ugly_val_release` and `ugly_error_payload` don't clear the ctx's error; every call that returns a status does.
  - A plugin reports a non-exception failure with `ugly_fail` (chosen status), an exception with `ugly_raise_native` (`RAISED` plus a payload), and a rethrown foreign value with `ugly_raise_handle`.
  - A Lua or Python error must never `longjmp` across a core frame or skip one: catch it at the boundary.
- **Build:** Meson (≥ 0.61) and Ninja, out-of-tree, with `-Wall` and no warnings. `meson test -C build --setup valgrind` fails on memory errors and on definite, indirect or possible leaks. `tests/valgrind.supp` suppresses only CPython's "possibly lost" live objects. Tests set `PYTHONMALLOC=malloc` and `PYTHONDONTWRITEBYTECODE=1`.
- **Tags:** `v0.01` is the original 2013–2014 code, including Java and JNA. `v0.02` is the v1 reference: the original contract with a Perl 5 plugin and XS binding, a Meson build and a TAP suite. v1 was removed from the tree on 2026-11-24 at the author's request.

## Embedding perl: what v1 taught, as applied in `plugins/perl5/`

- Every entry into an embedded perl must save and restore `PERL_GET_CONTEXT`.
- Call `PERL_SYS_INIT3` once per process, only if `PL_curinterp` is NULL. **Never** call `PERL_SYS_TERM`: after it, perl can't start again in the same process (`perl_alloc` exits with EBADF).
- Delete a runtime with `perl_destruct`/`perl_free` under its context.
- `call_sv` mortalizes its arguments. Whatever wraps an RV must own exactly one reference (v1's `wrap` took two, so objects were never DESTROYed).

## State (2026-11-24)

- **`master`**: the v2 core, plugins for the mock, Lua, CPython, JS (QuickJS) and Perl 5, bindings for Lua, Python, JS (qjs) and Perl, the conformance suite (40 runs: 8 groups × 5 plugins), pairwise (20 ordered pairs), the binding tests (4) and two examples. 66 tests pass, clean under valgrind, and helgrind finds no races.
- Process: work on a branch, open a PR, rebase-merge it, delete the branch. The author lets Claude push and merge at will. GitHub's "auto-delete head branches" setting is off, so delete branches explicitly.
- **The bindings milestone is reached (2026-11-24):** a plain `python3` program uses Lua modules, and a plain `lua5.4` program uses Python modules.
- **The plan in contract.md is complete** (2026-11-24): QuickJS, the JS binding, and Perl with its binding are done. Perl's open questions were decided with Claude's recommendations (contract decisions 9–12). Decision 6, Latin-1 decoding, was **revised** so that bytes round-trip; it's flagged to the author in the contract.
- **Next:** whatever the author picks from `TODO.md`. The HAProxy and embedded-Lua items are the motivating ones: any-thread runtimes, lazy Python init, and Lua 5.3/LuaJIT. CI and Node.js (N-API) are also there.
- **`TODO.md`** collects everything else: the HAProxy/embedded-Lua items (any-thread runtimes, lazy Python init for forking hosts, Lua 5.3 and 5.1/LuaJIT, blocking), what v2.0 left out (keyword arguments, iterators, hashes...), CI, and docs. Keep it current.

## Landscape (2026)

| Approach | What covers it today |
|---|---|
| In-process polyglot runtime | GraalVM Polyglot/Truffle (requires their VM and their language implementations) |
| Language-neutral interfaces + ABI | Wasm Component Model / WIT, WASI 0.2+ (requires compiling to Wasm, which shuts out native CPython/Perl/JVM ecosystems) |
| Plugin hosts | Extism, Wasmtime embeddings |
| Java ↔ native | Panama FFM API (final since Java 22) replaces JNI and JNA |
| Pairwise bridges | Inline::*, PyO3, JPype, napi-rs |
| Out-of-process | gRPC, Cap'n Proto, JSON-RPC |

**UGLY's niche** is embedding the *real* interpreters in one process and letting them call each other through a neutral C core. That niche is still largely unfilled.

## Docs

- `README.md` is the only README.
- `doc/design-essay.md` is in the author's first-person voice; the author verified its Inline::Java claims on 2026-11-24. It describes the 2013 design historically.
- `doc/contract.md` is the v2 spec. It is kept current: record any change to the contract in it, and in the headers.
