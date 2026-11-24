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
- **`lang/<x>/`**: language **plugins**. Each embeds that language's interpreter or VM and implements: runtime_new, load_library, call_function, object_new, object_call_method, object_delete.
  - Each plugin has helper code in its own language (`helper/helper.pm`), loaded at runtime from `UGLY_LANGUAGE_DIR/<lang>`. That path is hardcoded to `/usr/lib/ugly`.
- **`lib/<x>/`**: host-language **bindings**, so code in that language can drive the C API. Perl uses XS (`lib/perl5/Ugly.xs`).
- **`test/`**: `make check` runs everything through automake's `tap-driver.sh`. That covers the C programs in `test/t/*.c` and the Perl binding's `lib/perl5/t/*.t`.
  - The C tests use `test/ugly_test.h`, a small vendored TAP header with `ok`, `is_long`/`is_double`/`is_string`, `no_error`/`has_error` and `done_testing`. There are no external dependencies.
  - `no_error` turns `UGLY_NOT_SUPPORTED`/`UGLY_NOT_IMPLEMENTED` into skips, so one suite can run against every plugin.
  - The plugin is chosen with `UGLY_TEST_LANG` (default `perl5`), and the test libraries `calc`/`oocalc` live in `test/<lang>/`.
  - `AM_TESTS_ENVIRONMENT` in `test/Makefile.am` sets `UGLY_LANGUAGE_DIR` to the source tree's `lang/`, plus `LD_LIBRARY_PATH` and `PERL5LIB`, so no install is needed.
- **`UGLY_LANGUAGE_DIR`**: the compiled-in default is `$(pkglibdir)`, and the environment variable overrides it. Helpers live at `<dir>/<lang>/helper/` (perl5: `helper/helper.pm`).
- **Perl XS binding**: built by MakeMaker from `lib/Makefile.am` (`all-local`), after `lib/c`.
- **Build:** autotools, with the generated files committed (regenerated 2026-11-24). Only in-tree builds work, because `gen_iface.sh` is run by relative path. Linking the Perl plugin needs `libperl-dev`. `UGLY_DEBUG=<n>` turns on debug tracing.
- **Tag `v0.01`**: the original 2013–2014 state, including the Java plugin (`lang/java`), the JNA binding (`lib/java`) and `test/java`.

## Current state (assessed 2026-11-24)

- **Perl 5 plugin** works end to end, including objects and two runtimes in one process. It is **frozen as the v1 reference**; don't extend it against the old contract. Bug fixes are fine.
  - Every entry into an embedded perl must save and restore `PERL_GET_CONTEXT` (`UGLY_PERL5_ENTER`/`LEAVE` in `ugly_perl5_priv.h`).
  - `perl_call_helper_function` mortalizes its arguments, so the object handle is passed with `SvREFCNT_inc`.
- **Java has been removed** (2026-11-24). It was scaffolding against JDK 1.7 and survives in tag `v0.01`.
- The Perl binding wraps context, value, language and runtime (`load_library` only).
- As of 2026-11-24 everything builds without warnings on GCC/perl 5.34, and `make check` passes 114/114.
- **Valgrind baseline (2026-11-24, by hand, valgrind 3.18, `PERL_DESTRUCT_LEVEL=2`):**
  - There are **0 memory errors** in all 5 C tests: no invalid reads or writes, and no uninitialised values.
  - `t/runtime` and `t/object` each report 183 "possibly lost" records, all inside libperl. They come from embedded interpreters that are never destroyed (there's no `ugly_runtime_delete`).
  - `t/language` has 1 definite leak: the `ugly_language` struct (`language.c:53`), because there's no language teardown.

### Known bugs
- `ugly_debug_args`, `ugly_type_to_string` and `ugly_runtime_to_string` use static buffers, so they aren't reentrant. `ugly_debug_args` can also overflow its buffer.
- There's no `ugly_runtime_delete` and no language teardown, so runtimes leak.

### Housekeeping
- Stray `.svn` directory in `test/perl5/`.
- Out-of-tree (VPATH) builds fail: `lib/c` runs `./gen_iface.sh`, which reads `ugly_api.h` relative to the current directory.
- Generated autotools output is committed (`configure`, `Makefile.in`, `autom4te.cache`, …).

## Where we left off (2026-11-24)

- Work is on branch **`revive-docs-and-tests`**, in 4 commits: Java removal, test suite and fixes, regenerated autotools, docs. It is **not pushed and not merged** into `master`. Tag `v0.01` is local only.
- **Paused at the author's request: a `make check-valgrind` target.** The plan:
  - Add `AC_PATH_PROG([VALGRIND], [valgrind])` (optional) to `configure.ac`.
  - In `test/Makefile.am`, rerun `check` with `LOG_COMPILER="$(LIBTOOL) --mode=execute $(VALGRIND) --error-exitcode=99 …"` and `PERL_DESTRUCT_LEVEL=2`. Possibly also wrap `T_LOG_COMPILER` (perl) for the binding tests.
  - Fail on memory errors only, not on leaks (`--errors-for-leak-kinds=none`, or a suppressions file for libperl), until `ugly_runtime_delete` and language teardown exist.
- A natural follow-up: add `ugly_runtime_delete` (perl: `perl_destruct`/`perl_free` under `UGLY_PERL5_ENTER`) and `ugly_language_delete` (`dlclose`). Then make leaks fail the valgrind target too.

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
   - `README.md` is the main README. `README` is a plain-text pointer that automake's GNU strictness requires.
   - `doc/design-essay.md` is in the author's first-person voice. Its Inline::Java claims still need the author's verification.
2. **Possibly later: a small, sharp v2** (not a broad product).
   - Keep the core architecture.
   - Plugin order: Lua first (contract problems show up undisguised), then embedded CPython (GIL and reentrancy), then QuickJS. Perl is ported last, as the neutrality stress test.
   - Use Panama FFM instead of JNI/JNA if Java returns. Creating the JVM still needs the JNI invocation API.
   - Make callbacks, ownership rules and structured errors first-class in `ugly_value` and the API from day one.
   - Replace autotools with Meson or CMake.
   - Fix the known bugs above.
   - Each new plugin must pass the existing `make check UGLY_TEST_LANG=<lang>` suite. That suite is the start of the conformance tests.
