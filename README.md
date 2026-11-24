# UGLY: Ultimate Glue LaYer

UGLY is a C library for calling between programming languages **inside one
process**. It embeds the real interpreters (currently perl) and routes
every cross-language call through a small, language-neutral C core.

The goal is to replace pairwise bridges with a hub. N languages need
N(N-1)/2 direct bridges, while a hub needs one adapter per language in each
direction, or 2N in total:

- a **plugin** lets the core drive a language: embed its runtime, load code,
  call functions, create objects and call methods;
- a **binding** lets code in that language drive the core.

Any language with a binding can then call any language with a plugin, without
either of them knowing about the other.

UGLY grew out of [Inline::Java](https://metacpan.org/dist/Inline-Java), a
Perl↔Java bridge by the same author. It was written in March 2013, with a few
commits in January 2014, and is an unfinished prototype. That original state,
including an early Java plugin and a JNA binding, is tagged `v0.01`. The design essay,
[From Inline::Java to UGLY](doc/design-essay.md), explains what it was trying
to generalize, what it left unsolved, and how it compares with GraalVM and the
Wasm Component Model today.

## What it looks like

This C program calls a method on a Perl object (taken from
[test/t/object.c](test/t/object.c), with error handling shortened):

```c
#include <ugly.h>

ugly_context  *ctx  = ugly_context_new();
ugly_language *lang = ugly_language_init(ctx, "perl5");   /* dlopen libugly_perl5.so */
ugly_runtime  *rt   = ugly_runtime_new(ctx, lang);        /* embedded perl interpreter */

ugly_runtime_load_library(ctx, rt, "oocalc");             /* require oocalc; */
ugly_object *o = ugly_object_new(ctx, rt, "oocalc", NULL, 0, NULL);

ugly_value *args[] = { ugly_value_new_long(1), ugly_value_new_long(2) };
ugly_value *v = ugly_object_call_method(ctx, o, UGLY_LONG, "add", args, 2, NULL);
if (ugly_context_has_error(ctx))
    fprintf(stderr, "%s\n", ugly_context_get_error_msg(ctx));
else {
    printf("%ld\n", ugly_value_get_long(v));             /* 3 */
    ugly_value_delete(v);
}

ugly_value_delete(args[0]);
ugly_value_delete(args[1]);
ugly_object_delete(ctx, o);
ugly_runtime_delete(ctx, rt);                             /* UGLY_IN_USE while objects are alive */
ugly_language_delete(ctx, lang);                          /* UGLY_IN_USE while runtimes are alive */
ugly_context_delete(ctx);
```

The API follows three conventions:

- **Errors go into the `ugly_context`, not exceptions.** Every call clears the
  context first and may set an error code and a message.
- **The caller states the type it expects back** (`UGLY_LONG` above). Dynamic
  languages don't declare return types, so it is the caller that decides how a
  Perl scalar should come back.
- **A `hint` string** is reserved for disambiguating overloaded methods in
  statically typed targets. It is unused so far.

## Architecture

```text
   callers (bindings)              libugly (C core)                callees (plugins)

   C program ─────────────┐   ugly_context  (errors)
                          ├─► ugly_value    (tagged union) ─────► libugly_perl5.so ─► embedded perl
   Perl  ── Ugly.xs ──────┘   ugly_object   (foreign handle)         helper.pm
                              dispatch via plugin vtable
```

- **`lib/c/`**: the core. `ugly.h` holds the types and `ugly_api.h` the public
  API. `gen_iface.sh` parses `ugly_api.h` to generate the plugin vtable
  (`ugly_iface.h`), so `ugly_api.h` must keep one function per line and a
  single-token return type.
- **`ugly_value`** is a tagged union of `BOOL`, `LONG`, `DOUBLE`, `CHAR`,
  `STRING` and `OBJECT`. Every value that crosses a boundary is one of these.
- **`ugly_language_init("x")`** `dlopen`s `libugly_x.so` and calls
  `_ugly_x_language_init` to get the plugin's vtable. The core's runtime and
  object functions dispatch through it.
- **`lang/<x>/`**: plugins. Most of each plugin is written in its own language
  (`lang/perl5/helper/helper.pm`) and is loaded at runtime from
  `/usr/lib/ugly/<x>/`. The Perl helper `use`s the Perl *binding* to wrap its
  return values, so a language's plugin and binding share code.
- **`lib/<x>/`**: bindings. Perl's is XS (`lib/perl5/Ugly.xs`).

## Status

| Part | State |
|---|---|
| C core | Works. Contexts, values, language loading, runtimes and objects. |
| Perl 5 plugin | Works end to end, including objects and several runtimes in one process. All 147 tests pass, and the C tests are clean under valgrind (no memory errors, no leaks). |
| Perl binding (XS) | Context, value, language and runtime (`load_library`). No `call_function` or objects yet. |

The Perl 5 code is frozen as the v1 reference. The next round of work starts
with a new contract (handles, ownership, callbacks, structured errors) and new
plugins for Lua, embedded CPython and QuickJS. Perl will be ported to that
contract last. The Java plugin and JNA binding were scaffolding and have been
removed; they survive in the `v0.01` tag. If Java comes back, it will use the
Panama FFM API.

Known bugs and gaps:

- The type system has no arrays, maps, bytes or null.
- There are no callbacks, no cross-runtime ownership rules, no structured
  errors and no thread safety.

The last two lines are not small gaps. They are the substance of the
[design essay](doc/design-essay.md).

## Building and testing

You need a C toolchain, Meson and Ninja, plus perl with `ExtUtils::Embed` and
its embedding library. On Debian/Ubuntu that's `meson`, `ninja-build` and
`libperl-dev`. There are no other dependencies. Valgrind is optional.

```sh
meson setup build
meson compile -C build      # libugly, the perl5 plugin, and the Perl XS binding
meson test -C build         # the C tests and the Perl binding tests
```

The tests need no install step. `meson install -C build` installs:

- `libugly` and the plugins into `libdir`;
- the plugin helpers under `<libdir>/ugly/<lang>/`, a location that
  `UGLY_LANGUAGE_DIR` overrides at runtime;
- the Perl binding into perl's site directory, which the `perl_installdir`
  option overrides.

### The test suite

Each test is a TAP stream, and Meson runs it with `protocol: 'tap'`. The full
output is in `build/meson-logs/testlog.txt`.

- `test/t/*.c` are C programs that exercise the API: contexts, values,
  language loading, runtimes and objects. They use `test/ugly_test.h`, a small
  self-contained TAP header. The tests that need a plugin run against the one
  selected by `UGLY_TEST_LANG`, and load test libraries (`calc`, `oocalc`)
  from `test/<lang>/`.
- `lib/perl5/t/*.t` test the Perl binding, driving the same plugin.

A plugin that answers `UGLY_NOT_SUPPORTED` or `UGLY_NOT_IMPLEMENTED` gets
skips rather than failures, so the same suite can run against every plugin.

```sh
UGLY_TEST_LANG=lua meson test -C build    # the same suite against another plugin
meson test -C build object                # one test
meson test -C build --suite c             # only the C tests
UGLY_DEBUG=3 meson test -C build          # with trace output in the log
meson test -C build --setup valgrind      # the C tests under valgrind
```

The valgrind setup fails a test on any memory error and on any definite,
indirect or possible leak, so the tests delete everything they create.
Memory that is still reachable at exit is allowed, because a plugin may keep
process-wide state for the life of the process. For example, perl can't be
shut down and started again in one process.

## License

GPL v3. See [COPYING](COPYING).
