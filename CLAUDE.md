# CLAUDE.md

## Project: UGLY (Ultimate Glue LaYer)

A C hub for in-process, multi-language interop, written in March 2013 (plus a few commits in January 2014). The author also wrote Perl's **Inline::Java**. UGLY was the attempt to generalize that Perl↔Java bridge into an N-language system: **2N adapters instead of N² pairwise bridges**.

Treat the author as a domain expert on language interop. Discuss design tradeoffs; don't explain JNI, callbacks or cross-runtime lifetimes.

## Architecture

- **`lib/c/`**: the core library (`libugly`).
  - `ugly.h` holds the types and `ugly_api.h` holds the public API. `ugly_api.h` is parsed by `gen_iface.sh` to generate `ugly_iface.h`, so keep its formatting rules: one token for the return type, one function per line.
  - `ugly_context`: errors go here (error code + message), not into exceptions. Every API call clears it first.
  - `ugly_value`: a tagged union of BOOL, LONG, DOUBLE, CHAR, STRING and OBJECT. Every value crossing a language boundary goes through it.
  - `ugly_language`: loads the plugin `libugly_<lang>.so` with `dlopen` and calls `_ugly_<lang>_language_init` to get the plugin's interface vtable.
  - `ugly_runtime`, `ugly_object`: generic wrappers that dispatch through that vtable.
- **`lang/<x>/`**: language **plugins**. Each embeds that language's interpreter or VM and implements: language_delete, runtime_new, runtime_delete, load_library, call_function, object_new, object_call_method, object_delete. The vtable is generated from the `UGLY_IFACE` lines in `ugly_api.h`, so a new `UGLY_IFACE` entry forces every plugin to implement it.
  - Each plugin has helper code in its own language (`helper/helper.pm`), loaded at runtime from `<UGLY_LANGUAGE_DIR>/<lang>/helper/`.
- **`lib/<x>/`**: host-language **bindings**, so code in that language can drive the C API. Perl uses XS (`lib/perl5/Ugly.xs`).
- **`test/`**: `meson test -C build` runs every test as TAP (`protocol: 'tap'`). That covers the C programs in `test/t/*.c` (suite `c`) and the Perl binding's `lib/perl5/t/*.t` (suite `perl`).
  - The C tests use `test/ugly_test.h`, a small vendored TAP header with `ok`, `is_long`/`is_double`/`is_string`, `no_error`/`has_error` and `done_testing`. There are no external dependencies.
  - `no_error` turns `UGLY_NOT_SUPPORTED`/`UGLY_NOT_IMPLEMENTED` into skips, so one suite can run against every plugin.
  - The plugin is chosen with `UGLY_TEST_LANG`. It is deliberately not set by Meson, so the caller's value passes through. Both the C tests (`ugly_test_lang()`) and the `.t` files default to `perl5`. The test libraries `calc`/`oocalc` live in `test/<lang>/`.
  - `test_env` in `test/meson.build` sets `UGLY_LANGUAGE_DIR` to the source tree's `lang/`. It also sets `LD_LIBRARY_PATH` to every plugin's build dir (each `lang/<x>/meson.build` appends to `plugins`/`plugin_dirs`) and `PERL5LIB`, so no install is needed.
  - `meson test -C build --setup valgrind` runs the C suite under valgrind. See "Where we left off".
- **`UGLY_LANGUAGE_DIR`**: the compiled-in default is `<prefix>/<libdir>/ugly`, and the environment variable overrides it. Helpers live at `<dir>/<lang>/helper/` (perl5: `helper/helper.pm`).
- **Perl XS binding**: no MakeMaker. `xsubpp` generates `Ugly.c` (`lib/perl5/meson.build`), and `lib/perl5/auto/Ugly/meson.build` builds it, so the module lands at `build/lib/perl5/auto/Ugly/Ugly.so`, where XSLoader finds it through `@INC`. Install goes to `$Config{installsitearch}`, which the `perl_installdir` option overrides.
- **Build:** Meson (≥ 0.61) and Ninja: `meson setup build && meson compile -C build`. Builds are out-of-tree. It uses `-Wall` (`warning_level=1`) and is warning-free. `gen_iface.sh <path to ugly_api.h>` is a `custom_target`. Linking the Perl plugin needs `libperl-dev`. `UGLY_DEBUG=<n>` turns on debug tracing.
- **Tag `v0.01`**: the original 2013–2014 state, including the Java plugin (`lang/java`), the JNA binding (`lib/java`) and `test/java`.
- **Tag `v0.02`** (annotated): the **v1 reference**. The Perl 5 plugin is frozen against the original contract, with the Meson build. Both tags are pushed.

## Current state (assessed 2026-11-24)

- **Perl 5 plugin** works end to end, including objects and two runtimes in one process. It is **frozen as the v1 reference**; don't extend it against the old contract. Bug fixes are fine.
  - Every entry into an embedded perl must save and restore `PERL_GET_CONTEXT` (`UGLY_PERL5_ENTER`/`LEAVE` in `ugly_perl5_priv.h`).
  - `perl_call_helper_function` mortalizes its arguments, so the object handle is passed with `SvREFCNT_inc`.
  - An `ugly_object` owns exactly one reference to its perl RV (XS `Ugly::Object::wrap`), which `object_delete` releases. Until 2026-11-24, `wrap` took a second reference, so perl objects were never DESTROYed.
  - `PERL_SYS_INIT3` runs once per process, only if `PL_curinterp` is NULL. A host perl (the binding) already owns that state. `PERL_SYS_TERM` is **never** called: after it, perl can't start again in the same process (`perl_alloc` exits with EBADF). So the perl `language_delete` is a no-op.
- **Teardown rule (2026-11-24):** `ugly_runtime_delete` and `ugly_language_delete` refuse with `UGLY_IN_USE` while live objects or runtimes remain, and leave the target intact. The core counts these (`nb_objects`, `nb_runtimes` in `ugly_priv.h`). Deleting NULL is a no-op for language, runtime and object. Cascading deletes or a handle registry is left as a v2 ownership decision.
- **Java has been removed** (2026-11-24). It was scaffolding against JDK 1.7 and survives in tag `v0.01`.
- The Perl binding wraps context, value, language and runtime (`load_library` only).
- As of 2026-11-24 everything builds without warnings (`-Wall`) on GCC 11/perl 5.34, and `meson test` passes 9 tests with 147 subtests.
- **Valgrind (2026-11-24, valgrind 3.18):** `meson test --setup valgrind` passes. There are 0 memory errors and 0 definite, indirect or possible leaks in all 5 C tests. About 7 KB is still reachable in `t/runtime` and `t/object`: perl's process-wide state, which is kept on purpose. This doesn't need `PERL_DESTRUCT_LEVEL`, because a MULTIPLICITY perl defaults to destruct level 1.

### Known issues
- `ugly_debug_args` returns a per-thread buffer, valid until the next call in the same thread, and cuts long output with "...". Every other to-string is either a literal or cached per value, object or runtime. This was fixed on 2026-11-24, along with a buffer overflow in `ugly_debug_args`.
- The Perl binding's typemap used to leave a pointer unset when it was passed undef. `get_language` and `load_library` had no `= NULL` PREINIT to hide that. The typemap now sets NULL itself (fixed 2026-11-24).

## Where we left off (2026-11-24)

- **`master`** (pushed, 2026-11-24) holds the whole revival: Java removal, the TAP test suite and fixes, docs, teardown, the v1 cleanup, the Meson port (which also removed `doc/git.howto`, at the author's request), and version 0.02. The head is tagged **`v0.02`**, the v1 reference. The branches `revive-docs-and-tests` and `meson-build` are fully merged.
- **The valgrind setup** (`add_test_setup('valgrind', ...)` in `test/meson.build`) runs only the C suite (`exclude_suites: ['perl']`), directly with no libtool wrappers.
  - A test fails on any memory error or any definite, indirect or possible leak. Still-reachable memory is allowed.
  - Checked with an injected heap overread and an injected leak: both show as ERROR (exit status 99).
  - It uses `--keep-debuginfo=yes`, because plugins are `dlclose`d.
- **Teardown** (`ugly_runtime_delete`, `ugly_language_delete`): the tests delete everything they create, and cover re-initializing a language in the same process.
- **Next, agreed:** the v2 contract, an API header plus a spec written before any Lua code. The author reviews the draft header.

### Missing for a real polyglot layer
- Arrays, maps, bytes and null values.
- **Callbacks**: the called language calling back into the caller.
- **Ownership / GC rules** for objects held across runtimes.
- **Structured errors**: a type, a message and the originating language, propagated like exceptions.
- Thread safety.

Inline::Java solved most of these for the Perl↔Java pair. The open problem is turning those solutions into language-neutral rules in the C core.

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

## Direction agreed

1. **Docs first (drafted 2026-11-24).**
   - `README.md` is the only README.
   - `doc/design-essay.md` is in the author's first-person voice. The author verified its Inline::Java claims on 2026-11-24.
2. **Possibly later: a small, sharp v2** (not a broad product).
   - Keep the core architecture.
   - Plugin order: Lua first (contract problems show up undisguised), then embedded CPython (GIL and reentrancy), then QuickJS. Perl is ported last, as the neutrality stress test.
   - Use Panama FFM instead of JNI/JNA if Java returns. Creating the JVM still needs the JNI invocation API.
   - Make callbacks, ownership rules and structured errors first-class in `ugly_value` and the API from day one.
   - Build: Meson (done on 2026-11-24).
   - Each new plugin must pass the existing suite (`UGLY_TEST_LANG=<lang> meson test -C build`). That suite is the start of the conformance tests.
