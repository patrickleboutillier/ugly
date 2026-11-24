# UGLY: Ultimate Glue LaYer

UGLY is a C library for calling between programming languages **inside one
process**. It embeds the real interpreters (currently Lua and CPython) and
routes every cross-language call through a small, language-neutral C core.

The goal is to replace pairwise bridges with a hub. N languages need
N(N-1)/2 direct bridges. A hub needs one adapter per language, and any two
languages with an adapter can then use each other's functions, objects and
exceptions without knowing about each other.

UGLY grew out of [Inline::Java](https://metacpan.org/dist/Inline-Java), a
Perl↔Java bridge by the same author. It was first written in 2013, and the
design essay, [From Inline::Java to UGLY](doc/design-essay.md), explains
what it set out to generalize, what that first version left unsolved, and
how the idea compares with GraalVM and the Wasm Component Model. The current
version implements the contract in [doc/contract.md](doc/contract.md), which
answers those open questions.

## What it looks like

A Lua function calls Python's `math.sqrt`. Then Python raises, and the error
crosses Lua back to C. This is
[examples/polyglot.c](examples/polyglot.c), with error handling shortened;
it is built and run by the test suite.

```c
#include <ugly.h>

ugly_ctx *ctx;            ugly_ctx_new(&ctx);
ugly_lang *py, *lua;      ugly_lang_load(ctx, "python", &py);  ugly_lang_load(ctx, "lua", &lua);
ugly_rt *prt, *lrt;       ugly_rt_new(ctx, py, &prt);          ugly_rt_new(ctx, lua, &lrt);

ugly_handle math;         ugly_rt_import(ctx, prt, "math", &math);             /* a Python module */
ugly_val sqrt;            ugly_get_member(ctx, math, "sqrt", UGLY_HANDLE, &sqrt);

const char *code = "return function(f, x) return 'sqrt(' .. x .. ') = ' .. f(x) end";
ugly_val show;            ugly_rt_eval(ctx, lrt, code, strlen(code), UGLY_HANDLE, &show);  /* a Lua function */

ugly_val args[] = { sqrt, ugly_int(2) }, out;
ugly_call(ctx, show.as.h, args, 2, UGLY_STRING, &out);    /* Lua calls Python */
puts(out.as.str.ptr);                                     /* sqrt(2) = 1.4142135623731 */

args[1] = ugly_int(-1);
if (ugly_call(ctx, show.as.h, args, 2, UGLY_STRING, &out) == UGLY_ERR_RAISED){
    const ugly_error *e = ugly_last_error(ctx);           /* [python] ValueError: math domain error */
    printf("[%s] %s: %s\n", ugly_error_language(e), ugly_error_type(e), ugly_error_message(e));
}
```

Inside Lua, `f` is a proxy for the Python function. Inside Python, a Lua
table or function is a proxy too. A value that travels back to its home
language arrives as the original object, never as a proxy of a proxy.

## Concepts

- **Primitives cross by value**: null, bool, int64, double, UTF-8 string and
  bytes (`ugly_val`). **Everything else crosses as a handle**: functions,
  objects, lists, maps, modules and classes. Each handle lives in its home
  runtime.
- **Handles speak one small protocol**: members, elements, call, invoke and
  new, plus `describe`. Any language can use any other language's values
  through it.
- **Ownership**: results are owned by the caller and released explicitly.
  Arguments are borrowed for the call, and keeping a borrowed handle after
  the call is detected, not undefined.
- **Errors** carry their language, type, message and the original
  exception object. When an error returns to its home language, it is
  rethrown as that same object.
- **Callbacks** are just callable handles. Calls can nest across languages,
  up to a limit, and going past the limit fails cleanly.
- **Threads**: a runtime belongs to the thread that created it.

## Layout

| Path | What |
|---|---|
| `include/ugly.h` | The public API. |
| `include/ugly_plugin.h` | The plugin contract: what a language plugin implements, and the core services it may use. |
| `src/` | The core (`libugly`): handle table, call frames and borrows, errors, threads, plugin loading. Also the built-in "c" language, which is the home of C callbacks and core lists and maps. |
| `plugins/mock/` | A small language written in C, used to test the core and the contract before any real interpreter. |
| `plugins/lua/` | Lua 5.4. |
| `plugins/python/` | CPython 3.10+. One runtime per process; Python is never finalized. |
| `tests/` | The conformance suite: the contract, as tests. Each plugin's fixtures are in `tests/<lang>/`. |
| `examples/` | Example programs, built and run as tests. |
| `doc/` | The contract and the design essay. |

## Status

Lua and CPython pass the whole conformance suite, and call each other in one
process. Next on the plan (in [doc/contract.md](doc/contract.md), "Order of
work"):

1. **Bindings for Lua and Python**: an `ugly` module in each language, plus
   attach mode, so that a plain `lua` or `python3` program can use the other
   language with no C host.
2. **QuickJS.**
3. **Perl**, ported from the original plugin.

History is in the tags. `v0.01` is the original 2013–2014 code, including an
early Java plugin and a JNA binding. `v0.02` is that first design brought
up to date: a Perl 5 plugin and an XS binding, with an autotools-free build
and a test suite. Neither is in the tree any more. If Java comes back, it
will use the Panama FFM API.

## Building and testing

You need a C toolchain, Meson (0.61 or later) and Ninja. Each language
plugin is built only if its language is found: Lua 5.4 (`liblua5.4-dev`)
and CPython 3.10+ (`python3-dev`). `-Dlua=disabled` and `-Dpython=disabled`
skip them explicitly. Valgrind is optional.

```sh
meson setup build
meson compile -C build
meson test -C build        # the conformance suite, pairwise tests and examples
```

The tests need no install step. `meson install -C build` installs the
library, the two headers, a pkg-config file and the plugins. Plugins are
looked up in `<libdir>/ugly/plugins/`, and `UGLY_PLUGIN_PATH` (a
colon-separated list of directories) overrides that at runtime.

### Tests

Every test is a TAP stream. The full output is in
`build/meson-logs/testlog.txt`.

- **Conformance** (`tests/*.c`): eight groups, which are values, protocol,
  ownership, identity, callbacks, errors, threads and lifecycle. Each group
  runs against every plugin built, as suite `conformance-<plugin>`. The
  fixtures are each plugin's `conformance` module, from `tests/<lang>/`.
- **Pairwise** (`tests/pairwise.c`): every ordered pair of different
  languages in one process, using each other's values, calls and errors.
- **Examples** (`examples/`): run, and checked by exit status.

```sh
meson test -C build --suite conformance-lua   # one plugin
meson test -C build --suite pairwise          # every pair of languages
meson test -C build --setup valgrind          # everything under valgrind
```

The valgrind setup fails a test on any memory error and on any definite,
indirect or possible leak, so the tests delete everything they create.
Memory that is still reachable at exit is allowed, because a plugin may keep
process-wide state for the life of the process. `tests/valgrind.supp`
suppresses one thing: CPython's live objects reported as "possibly lost",
because Python is never finalized.

## License

GPL v3. See [COPYING](COPYING).
