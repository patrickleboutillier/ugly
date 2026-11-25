# CLAUDE.md

## Project: UGLY (Ultimate Glue LaYer)

A C hub for in-process, multi-language interop. It embeds the real interpreters and routes every cross-language call through a neutral C core. It was first written in 2013–2014 by the author of Perl's **Inline::Java**, as an attempt to generalize that Perl↔Java bridge to N languages: one adapter per language instead of N² pairwise bridges. It was revived in 2026 around a new contract, `doc/contract.md`.

Treat the author as a domain expert on language interop. Discuss design tradeoffs; don't explain JNI, callbacks or cross-runtime lifetimes.

## Architecture

- **`doc/contract.md`** is the spec. It was approved on 2026-11-24 (PR #1), and its "Decisions" and "Clarifications" sections record everything settled since. Every rule in it should be a test in `tests/`.
- **`include/ugly.h`** is the public API, and **`include/ugly_plugin.h`** is the plugin contract (the plugin rules plus the core services). The plugin vtable is written by hand; nothing is generated.
- **`src/`** is the core (`libugly` 2.4.0, plugin ABI 4):
  - `handles.c`: the handle table. A handle is a slot index plus a generation. It also holds borrow slots, the (home, identity) index and the per-runtime queues of deferred releases, all under one lock. Plugin code never runs under that lock.
  - `ctx.c`: one ctx per thread (`_Thread_local current`) and errors.
  - `rt.c`: runtimes and frames. `rt_enter`/`rt_leave` check the thread, enforce `UGLY_MAX_DEPTH`, drain deferred releases and revoke borrows. `after_call` normalizes plugin results.
  - `protocol.c`: the operations.
  - `lang.c`: plugin loading through `UGLY_PLUGIN_PATH`, with `RTLD_GLOBAL` and counted loads.
  - `host.c`: the built-in "c" language, which is the home of C callables and core lists and maps. Each ctx has one runtime of it.
- **`plugins/<lang>/`** holds the plugins, built as `ugly_<lang>.so` and exporting `ugly_plugin_<lang>()`. Each one's `meson.build` appends to `plugins`, `plugin_dirs` and `test_plugins`, and is optional (`-D<lang>=`).
  - **`mock/`** is a small language in C (lenient, Perl-like conversions) that implements every plugin rule and every fixture.
  - **`lua/`** is Lua 5.4 or 5.3 (`-Dlua_version=auto|5.4|5.3`; one Lua per process, language `"lua"` either way). The binding is built for every Lua found, in `binding/<version>/` (identical meson files); tests are `binding-lua5.4` and `binding-lua5.3`. A `LUA_VERSION_NUM` shim covers 5.3.
    - Every operation runs under `lua_pcall` (`op_body`), except `call`, which pcalls the target directly so a call costs one of Lua's roughly 200 nested C calls.
    - Proxies are `ugly.proxy` userdata, cached per owned handle in a weak table. A foreign function (CALLABLE, not INSTANTIABLE) is a C closure (`proxy_fn`) over that userdata instead, so Lua's C library takes it; `to_proxy()` recognizes both. A callable member that isn't a class becomes a method closure (`proxy_method`); a class member stays the class. Foreign errors travel as `ugly.error` userdata, with `e.payload`, and are restored unchanged when they escape.
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
    - Refs are SVs. Foreign values are `Ugly::Proxy`: a blessed scalar ref holding the handle, cached per handle. Its Perl side (overloads, `AUTOLOAD`, complete tied hash and array, and `Ugly::import`) is `helper.pm`, turned into the C string `helper_pm` at build time by `embed.pl` and defined in every interpreter; the XS functions are registered with `newXS`.
    - `use Ugly python => 'json'` binds a module to a package (`bind_module`): callable members become subs on first use (`AUTOLOAD`), classes nested packages with `new`. An `AUTOLOAD` closure sees `$Ugly::AUTOLOAD`, the package it was compiled in, not the target's.
    - Modules and classes are `Ugly::Package` objects (a scalar ref holding the package name). Their identity is the stash, members are subs and nested packages, and `new` instantiates.
    - Calls run in list context and collapse (`call_perl`). Never `PUSHMARK` before `eval_sv`: it pushes its own mark.
    - The binding (`binding/`: `lib/Ugly.pm`, `auto/Ugly/Ugly.so`, `boot_Ugly`) attaches to the running perl. `END { Ugly::_teardown() }` tears down. Its test sets `PERL_DESTRUCT_LEVEL=2`, so the host frees everything and valgrind measures only UGLY.
  - **`java/`** is Java through JNI (JDK 21+, found via `javac`; `-ljvm` with an rpath to `lib/server`).
    - One JVM per process, created in `init` (or the running one reused), with `-Xrs` and the class path `<plugin dir>/ugly.jar:$UGLY_JAVA_CLASSPATH:$CLASSPATH`. Only `UGLY_CAP_ATTACH`.
    - Most of the protocol is Java, in `ugly/Bridge.java`: reflection, bean getters, `Map` keys, arrays and `List`s, and overloads chosen by `score()`. A foreign value passed for an interface becomes a dynamic proxy (`ForeignHandler`).
    - `ugly.Foreign` holds a handle, released by a `Cleaner` and cached per handle; its natives are registered from `java.c`. Errors from elsewhere are `ugly.ForeignException`, which owns the core's error until it leaves Java.
    - Refs are global refs; identity is an id from `Bridge.acquireId` (an `IdentityHashMap`), since objects move. Strings are converted from UTF-16 in C, never via modified UTF-8.
    - Every op runs in `FRAME_IN`/`FRAME_OUT`, and every JNI call that can throw is followed by `JCHECK` or `drop_exception`.
    - JVM output goes to stderr (the `vfprintf` hook). `UGLY_JAVA_CHECK` adds `-Xcheck:jni` and aborts on a JNI warning at `FRAME_OUT`; the tests set it. Its signal checker prints "handler modified" noise at `exit()`, harmlessly, now on stderr.
    - Java tests are in suite `jvm`, which the valgrind setup excludes. The fixture is `tests/java/conformance.java`, built into `conformance.jar`.
    - The binding (`binding/`, `libugly_jni.so`, no libjvm) is loaded by `ugly.Foreign`'s static initializer unless `-Dugly.embedded=true` (set by the plugin when it creates the JVM). `JNI_OnLoad` registers and attaches on the loading thread; teardown is at JVMTI VMDeath, which runs on `main`'s thread (shutdown hooks run on new threads, which own no ctx). Its test runs `java` with `-Xcheck:jni -XX:+DisplayVMOutputToStderr`, and its last TAP line comes from a Lua finalizer at teardown.
  - Proxies in all five languages turn a callable member into `ugly_invoke`, so `obj.m(x)` uses the foreign language's receiver rules.
  - **Bindings** (`plugins/<lang>/binding/`) compile the plugin's own source again with `-DUGLY_BINDING`, as the native module `ugly`: `luaopen_ugly`, `PyInit_ugly` via `pyinst.extension_module`, and Java's `JNI_OnLoad` in `libugly_jni.so`. They link **no interpreter library**, because Debian's `python3` and `lua5.4` are statically linked and a second interpreter in the process would be fatal.
    - On import, a binding calls `ugly_lang_register` (no dlopen) and `ugly_rt_attach` on the host interpreter. The Lua binding does it on the first use of `ugly` instead (`binding_attach`), on that thread, attaching the state's main thread: HAProxy loads states on its main thread and runs them on workers. Teardown is a `__gc` sentinel in Lua and `atexit` in Python.
    - Every runtime, embedded or attached, preloads the same `ugly` module: `ugly.module(lang, name)` and `ugly.eval(lang, code)`, which use `ugly_rt_get`, plus `is_error`/`is_proxy` in Lua and `Error`/`Proxy`/`rethrow` in Python.
    - The Lua plugin keeps its runtime in the registry (`ugly.rt`), not `LUA_EXTRASPACE`, and converts on the `lua_State` it was given, so it works inside coroutines.
  - Rule 7 is complete: an error whose payload comes home is raised as the original object (`throw_ugly_error`, `raise_ugly`).
- **`tests/`** is the conformance suite: 9 C programs (the 8 groups of contract §9, and `options`) plus `conformance.h` (TAP and fixture), and `pairwise.c`.
  - Meson registers each program once per plugin in `test_plugins` (suites `conformance` and `conformance-<plugin>`, env `UGLY_TEST_PLUGIN`), and `pairwise.c` once per ordered pair of different plugins (suite `pairwise`, env `UGLY_TEST_PEER`).
  - Fixtures are each plugin's `conformance` module, in `tests/lua/` and `tests/python/` (the mock's are built in). The spec is in contract §9.
  - Expectations that depend on the language are "exact or refused". Tests that need a capability a plugin lacks report a SKIP; they never drop checks silently.
- **`examples/`**: programs built and run as tests by exit status, which README quotes. `haproxy/` is the HAProxy proof of concept (`poc.lua`, `policy.py`, `haproxy.cfg` with `@PORT@`/`@LUA@`/`@NBTHREAD@`, and `run_poc.py`, TAP): run when `haproxy` is installed, against the binding for HAProxy's Lua (from `haproxy -vv`), with `lua-load` on 1 and 4 threads and `lua-load-per-thread` on 4; suite `haproxy`, kept out of valgrind and helgrind. `python_from_lua.lua` is a plain Lua script using Python's standard library through the binding (README's lead example). `polyglot.c` is a C host using `ugly_rt_get`.
- **Binding tests** (suite `bindings`): `tests/python/test_binding.py` runs under the `python3` that Meson found, and `tests/lua/test_binding.lua` under `lua5.4`, and `tests/java/test_binding.java` under `java` (suites `bindings` and `jvm`). All are TAP, and the Lua one ends with `os.exit(code, true)`, so the state closes and the binding tears down.
- Rules that are easy to get wrong:
  - Arguments are borrowed and revoked on return, so a plugin must `ugly_dup` what it keeps. Results are owned.
  - Threads (contract §6): `UGLY_CAP_ANY_THREAD` (Python, Java) lets any thread enter; `rt_enter` counts itself in (`depth`, atomic) before checking `closing`, `rt_destroy` sets `closing` before checking `depth`. Releases to such runtimes are always queued. `ugly_rt_get` shares them (`shares`, `auto_created`, `destroyed` under the langs lock; `rt_unshare`). `ugly_ctx_thread` makes a ctx deleted at thread exit (pthread key); Python's `need_ctx` and Java's `native_ctx` use it. Python wraps calls out in `OUT()` (GIL released). Phase 2: `ugly_rt_set_host_serialized` makes one runtime any-thread on the host's promise (HAProxy's `lua-load`; Lua's `ugly.shared_by_threads()`, called when `core.thread == 0`); the Lua binding calls `ugly_rt_drain` at each entry; QuickJS resets its stack top on outermost entry.
  - The plugin ABI is 4: `delete_member` and `splice` (2), `options` (3), `call_opts`/`invoke_opts`/`new_opts` (4). Call options (contract §11) carry keyword arguments, a hint and a context in an `ugly_opts` (`UGLY_OPTS_INIT`; `size` first, so older callers' smaller structs work); the core borrows keyword values and refuses options for a plugin without `_opts` entries. Python takes keyword arguments; Perl and the mock take them as trailing pairs, and Perl a context; the others refuse. Bindings: native on Python proxies; a trailing marker elsewhere (`ugly.kw{}` in Lua, sorted by name; `ugly.kw({})` in JS, an `ugly.Kw` object; `Ugly::kw()` in Perl, an `Ugly::Kw` arrayref; `Ugly.kw()` in Java, `ugly.Kw`, split out on the Java side before `callKw`/`invokeKw`). Calling a proxy of a class that's INSTANTIABLE but not CALLABLE constructs it (Python caches the traits in the proxy). Hints and contexts: an `opts` marker, also passed last, beside `kw` in either order (`ugly.opts`, `Ugly::opts`, `Ugly.opts().hint(...)`); Java's hint is a per-thread "next hint" in `Bridge`, consumed by the call's overload resolution (`choose`). The core range-checks `splice` with `length` before the plugin runs.
  - `ugly_configure` (options, `lang.c`) is refused once a language is in the `langs` list, which a plugin without `term` never leaves. Plugins read options with `ugly_option` in `init` (Python, Java) or `rt_new` (Lua, Perl), or at module load (JS); `init` runs under `langs_lock`, so options have their own lock.
  - `ugly_release`, `ugly_val_release` and `ugly_error_payload` don't clear the ctx's error; every call that returns a status does.
  - A plugin reports a non-exception failure with `ugly_fail` (chosen status), an exception with `ugly_raise_native` (`RAISED` plus a payload), and a rethrown foreign value with `ugly_raise_handle`.
  - A Lua or Python error must never `longjmp` across a core frame or skip one: catch it at the boundary.
  - Signals belong to whoever installed them first: a plugin restores any handler its interpreter resets while starting (perl ignores SIGFPE, which the JVM needs).
- **CI** (`.github/workflows/ci.yml`): all languages on Ubuntu 22.04 and 24.04 (`-Dwerror=true`, test, valgrind, helgrind on `threads-*`), one job per disabled language, and a core-plus-mock job. JDK 21 is the runner's Temurin (`JAVA_HOME_21_X64`). Binding tests get the built plugins in `UGLY_TEST_LANGS` and SKIP checks whose language isn't built.
- **Build:** Meson (≥ 0.61) and Ninja, out-of-tree, with `-Wall` and no warnings. `meson test -C build --setup valgrind` fails on memory errors and on definite, indirect or possible leaks. `tests/valgrind.supp` suppresses only CPython's "possibly lost" live objects. Tests set `PYTHONMALLOC=malloc` and `PYTHONDONTWRITEBYTECODE=1`.
- **Tags:** `v0.01` is the original 2013–2014 code, including Java and JNA. `v0.02` is the v1 reference: the original contract with a Perl 5 plugin and XS binding, a Meson build and a TAP suite. v1 was removed from the tree on 2026-11-24 at the author's request.

## Embedding perl: what v1 taught, as applied in `plugins/perl5/`

- Every entry into an embedded perl must save and restore `PERL_GET_CONTEXT`.
- Call `PERL_SYS_INIT3` once per process, only if `PL_curinterp` is NULL. **Never** call `PERL_SYS_TERM`: after it, perl can't start again in the same process (`perl_alloc` exits with EBADF).
- Delete a runtime with `perl_destruct`/`perl_free` under its context.
- `call_sv` mortalizes its arguments. Whatever wraps an RV must own exactly one reference (v1's `wrap` took two, so objects were never DESTROYed).

## State (2026-11-25)

- **`master`**: the v2 core, plugins for the mock, Lua, CPython, JS (QuickJS), Perl 5 and Java, bindings for Lua, Python, JS (qjs), Perl and Java, the conformance suite (48 runs: 8 groups × 6 plugins), pairwise (30 ordered pairs), the binding tests (7) and two examples. 87 tests pass; the 66 without a JVM are clean under valgrind, and helgrind finds no races.
- Releases: source tarballs only (`.github/workflows/release.yml`). To release: set the version in `meson.build` (and `src/meson.build`'s library version if the ABI changed), merge, tag `vX.Y.Z` on master, push the tag; CI checks the tag against the version, runs `meson dist` (build and test from the tarball) and publishes. `gh workflow run release.yml` is a dry run. Releasing is outward-facing: ask the author first.
- Process: work on a branch, open a PR, rebase-merge it, delete the branch. The author lets Claude push and merge at will. GitHub's "auto-delete head branches" setting is off, so delete branches explicitly.
- **The bindings milestone is reached (2026-11-24):** a plain `python3` program uses Lua modules, and a plain `lua5.4` program uses Python modules.
- **The plan in contract.md is complete** (2026-11-24): QuickJS, the JS binding, and Perl with its binding are done. Perl's open questions were decided with Claude's recommendations (contract decisions 9–12). Decision 6, Latin-1 decoding, was **revised** so that bytes round-trip; it's flagged to the author in the contract.
- **Java (2026-11-25)** was the first language after the plan, with its binding.
- **Done since (2026-11-25):** Java and its binding, Perl's namespace import and ties (with `ugly_splice`/`ugly_delete_member`), `ugly_configure`, CI, foreign functions as Lua functions, Lua 5.3, source releases (none published yet: the author tags), any-thread runtimes phase 1.
- **Next:** whatever the author picks. The HAProxy POC works in both Lua modes on 4 threads, with one Python.
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
