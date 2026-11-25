# UGLY: Ultimate Glue LaYer

UGLY is a C library for calling between programming languages **inside one
process**. It embeds the real interpreters (Lua, CPython, QuickJS,
Perl 5 and the JVM) and routes every cross-language call through a small, language-neutral C core.

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

A plain Lua script using Python's standard library. This is
[examples/python_from_lua.lua](examples/python_from_lua.lua), which the
test suite runs:

```lua
local ugly = require "ugly"
local python = function(name) return ugly.module("python", name) end

local json, statistics = python "json", python "statistics"
local functools, datetime = python "functools", python "datetime"

-- Python parses JSON; Lua reads the dict and the list it gives back.
local data = json.loads('{"name": "ugly", "scores": [3, 1, 4, 1, 5]}')
print(data.name, #data.scores, data.scores[1])     --> ugly    5    3

-- Python computes on its own list...
print(statistics.mean(data.scores))                 --> 2.8

-- ...and on a Lua table, calling back into a Lua function.
print(functools.reduce(function(a, b) return a * b end, { 1, 2, 3, 4, 5 }))   --> 120

-- Python objects have methods.
local day = datetime.date(2026, 11, 24)
print(day:isoformat(), day.year)                    --> 2026-11-24    2026

-- Python exceptions arrive as errors, with their type and message.
local ok, err = pcall(json.loads, "{oops")
print(ok, err.type)                                 --> false   JSONDecodeError
```

Inside Lua, a Python object is a proxy. Inside Python, a Lua table or
function is a proxy too, which is how `functools.reduce` iterates the table
and calls the function. A value that travels back to its home language
arrives as the original object, never as a proxy of a proxy. Errors keep
their identity too: an exception raised in Python that crosses Lua is caught
in Python as the same object.

### From Python

The other way round works the same, with `import ugly` in a plain `python3`
program. These lines come from
[tests/python/test_binding.py](tests/python/test_binding.py):

```python
import ugly

string = ugly.module("lua", "string")             # a Lua module
string.upper("abc")                               # 'ABC'

lua = ugly.module("lua", "conformance")
lua.apply(lambda x: x * 2, 21)                    # 42: Lua calls a Python lambda
c = lua.make_counter()                            # a Lua object
c.inc(); c.count                                  # 1
```

Code reached this way can use `ugly` too, and it reaches back into the
host's own interpreter. For example, Lua called from that Python program
can `require "ugly"` and use the program's own globals.

A QuickJS program (`qjs`) works the same way, by importing the native module
([tests/js/test_binding.js](tests/js/test_binding.js)):

```js
const ugly = await import("/path/to/ugly.so");
const py = ugly.module("python", "conformance");
py.apply(x => x * 2, 21);                         // 42: Python calls a JS arrow function
ugly.module("lua", "string").upper("abc");        // "ABC"
```

A plain `java` program uses `ugly.Ugly`, with `ugly.jar` on the class path
and `libugly_jni.so` on `java.library.path`
([tests/java/test_binding.java](tests/java/test_binding.java)):

```java
import ugly.Foreign;
import ugly.Ugly;

Foreign math = (Foreign)Ugly.module("python", "math");
math.invoke("sqrt", 16.0);                        // 4.0
Foreign lua = (Foreign)Ugly.module("lua", "conformance");
Function<Object, Object> twice = x -> (Long)x * 2;
lua.invoke("apply", twice, 21);                   // 42: Lua calls a Java lambda
Foreign f = (Foreign)Ugly.eval("lua", "return function(l) l:replaceAll(function(x) return x * 10 end) end");
f.call(list);                                     // a Lua function as Java's UnaryOperator
```

```sh
java -cp ugly.jar:. -Djava.library.path=<libdir> Main
```

### From C

A C program can host any languages itself and pass values between them.
[examples/polyglot.c](examples/polyglot.c) gives Python's `math.sqrt` to a
Lua function, and gets Python's error back when it fails (error handling
shortened here):

```c
#include <ugly.h>

ugly_ctx *ctx;            ugly_ctx_new(&ctx);
ugly_rt *prt, *lrt;       ugly_rt_get(ctx, "python", &prt);    ugly_rt_get(ctx, "lua", &lrt);

ugly_handle math;         ugly_rt_import(ctx, prt, "math", &math);
ugly_val sqrt;            ugly_get_member(ctx, math, "sqrt", UGLY_HANDLE, &sqrt);

const char *code = "return function(f, x) return 'sqrt(' .. x .. ') = ' .. f(x) end";
ugly_val show;            ugly_rt_eval(ctx, lrt, code, strlen(code), UGLY_HANDLE, &show);

ugly_val args[] = { sqrt, ugly_int(2) }, out;
ugly_call(ctx, show.as.h, args, 2, UGLY_STRING, &out);    /* Lua calls Python */
puts(out.as.str.ptr);                                     /* sqrt(2) = 1.4142135623731 */

args[1] = ugly_int(-1);                                   /* Python raises, across Lua */
if (ugly_call(ctx, show.as.h, args, 2, UGLY_STRING, &out) == UGLY_ERR_RAISED){
    const ugly_error *e = ugly_last_error(ctx);           /* [python] ValueError: math domain error */
    printf("[%s] %s: %s\n", ugly_error_language(e), ugly_error_type(e), ugly_error_message(e));
}
```

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
| `plugins/lua/` | Lua 5.4. `binding/` builds the same source as the Lua module `ugly`. |
| `plugins/python/` | CPython 3.10+. One runtime per process; Python is never finalized. `binding/` builds the same source as the Python extension `ugly`. |
| `plugins/js/` | JavaScript on QuickJS, built from a pinned release (`subprojects/quickjs.wrap`). ES modules are loaded from `UGLY_JS_PATH`. `binding/` builds the same source as the native module `ugly.so` for `qjs`. |
| `plugins/perl5/` | Perl 5. Modules and classes are packages; calls run in list context. `binding/` builds the same source as the XS module `Ugly` (`use Ugly`). |
| `plugins/java/` | Java through JNI. One JVM per process; classes are imported by name and their overloads chosen by how well the arguments convert. The helper classes (`ugly.Foreign`, `ugly.ForeignException`, `ugly.Bridge`) are `ugly.jar`. `binding/` builds the same source as `libugly_jni.so`, which `ugly.Foreign` loads in a plain `java` program. |
| `tests/` | The conformance suite: the contract, as tests. Each plugin's fixtures are in `tests/<lang>/`. |
| `examples/` | Example programs (Lua using Python, and a C host), run as tests. |
| `doc/` | The contract and the design essay. |

## Status

Lua, CPython, JavaScript (QuickJS), Perl 5 and Java pass the whole
conformance suite, and every pair of them works in one process. Each one can
also be the host: plain `lua`, `python3`, `qjs`, `perl` and `java` programs
use the other languages. The plan in [doc/contract.md](doc/contract.md) is
complete, and Java came after it.

Other ideas and open work, including embedding in hosts like HAProxy that
have Lua built in, are in [TODO.md](TODO.md).

History is in the tags. `v0.01` is the original 2013–2014 code, including an
early Java plugin and a JNA binding. `v0.02` is that first design brought
up to date: a Perl 5 plugin and an XS binding, with an autotools-free build
and a test suite. Neither is in the tree any more. Java came back in v2
through JNI, which JDK 21 still needs; the Panama FFM API is final from
JDK 22.

## Building and testing

You need a C toolchain, Meson (0.61 or later) and Ninja. Each language
plugin, and its binding, is built only if its language is found: Lua 5.4
(`liblua5.4-dev`), CPython 3.10+ (`python3-dev`), Perl 5 (`libperl-dev`)
and a JDK, 21 or later (`openjdk-21-jdk-headless`, found through `javac`).
QuickJS is built from source: Meson downloads its pinned release at setup,
and checks the checksum. `-Dlua=disabled`, `-Dpython=disabled`,
`-Djs=disabled`, `-Dperl5=disabled` and `-Djava=disabled` skip a language;
`--wrap-mode=nodownload` forbids the download. The Lua binding
test also needs the `lua5.4` interpreter. Valgrind is optional.

```sh
meson setup build
meson compile -C build
meson test -C build        # the conformance suite, pairwise tests and examples
```

The tests need no install step. `meson install -C build` installs the
library, the two headers, a pkg-config file, the plugins and the bindings
(the Lua module where Lua looks for C modules, the Python extension in
site-packages, the Java one in libdir). Plugins are looked up in `<libdir>/ugly/plugins/`, and
`UGLY_PLUGIN_PATH` (a colon-separated list of directories) overrides that at
runtime.

### Tests

Every test is a TAP stream. The full output is in
`build/meson-logs/testlog.txt`.

- **Conformance** (`tests/*.c`): eight groups, which are values, protocol,
  ownership, identity, callbacks, errors, threads and lifecycle. Each group
  runs against every plugin built, as suite `conformance-<plugin>`. The
  fixtures are each plugin's `conformance` module, from `tests/<lang>/`.
- **Pairwise** (`tests/pairwise.c`): every ordered pair of different
  languages in one process, using each other's values, calls and errors.
- **Bindings** (`tests/<lang>/test_binding.*`): plain `python3`, `lua5.4`,
  `qjs`, `perl` and `java` programs using the other languages through `ugly`, in
  attach mode. The `qjs` is the one built from the QuickJS subproject.
- **Examples** (`examples/`): run, and checked by exit status. They're the
  programs README quotes.

```sh
meson test -C build --suite conformance-lua   # one plugin
meson test -C build --suite pairwise          # every pair of languages
meson test -C build --setup valgrind          # everything under valgrind
```

The valgrind setup fails a test on any memory error and on any definite,
indirect or possible leak, so the tests delete everything they create.
Memory that is still reachable at exit is allowed, because a plugin may keep
process-wide state for the life of the process. `tests/valgrind.supp`
suppresses one thing: CPython's live objects reported as "possibly lost"
(from `libpython`, or from a statically linked `python3` host), because
Python is never finalized. Tests with a JVM in them (suite `jvm`) don't run
under valgrind: its JIT and GC are beyond what memcheck can judge. They run
with `-Xcheck:jni` instead (`UGLY_JAVA_CHECK`), where a JNI warning aborts.

## License

GPL v3. See [COPYING](COPYING).
