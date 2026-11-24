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
  - Proxies in both languages turn a callable member into `ugly_invoke`, so `obj.m(x)` uses the foreign language's receiver rules.
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

## Notes for the Perl port (learned on v1, tag `v0.02`)

- Every entry into an embedded perl must save and restore `PERL_GET_CONTEXT` (v1's `UGLY_PERL5_ENTER`/`LEAVE` in `lang/perl5/ugly_perl5_priv.h`).
- Call `PERL_SYS_INIT3` once per process, and only if `PL_curinterp` is NULL; a host perl already owns that state. **Never** call `PERL_SYS_TERM`: after it, perl can't start again in the same process (`perl_alloc` exits with EBADF). So Perl isn't RESTARTABLE.
- Delete a runtime with `perl_destruct`/`perl_free` under the interpreter's context. A MULTIPLICITY perl defaults to destruct level 1, so `PERL_DESTRUCT_LEVEL` isn't needed.
- `call_sv` with mortalized arguments: anything that must outlive the call needs `SvREFCNT_inc`. A wrapper must own exactly one reference to its RV; v1's `wrap` took two, so objects were never DESTROYed.
- In XS typemaps, set the C variable to NULL in the `else` branch for undef, or functions without a NULL PREINIT read garbage.
- The Perl helper (`helper.pm`) `use`d the binding to share value code. In v2 terms, the plugin and the binding are one adapter.

## State (2026-11-24)

- **`master`**: the v2 core, plugins for the mock, Lua and CPython, bindings for Lua and Python, the conformance suite (24 runs: 8 groups × 3 plugins), pairwise (6 ordered pairs), the binding tests (2) and the polyglot example. 33 tests pass, clean under valgrind, and helgrind finds no races.
- Process: work on a branch, open a PR, rebase-merge it, delete the branch. The author lets Claude push and merge at will. GitHub's "auto-delete head branches" setting is off, so delete branches explicitly.
- **The bindings milestone is reached (2026-11-24):** a plain `python3` program uses Lua modules, and a plain `lua5.4` program uses Python modules.
- **Next: QuickJS**, then the Perl port (see the notes above). See contract.md "Order of work".

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
