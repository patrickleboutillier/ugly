# UGLY: Ultimate Glue LaYer

[![CI](https://github.com/patrickleboutillier/ugly/actions/workflows/ci.yml/badge.svg)](https://github.com/patrickleboutillier/ugly/actions/workflows/ci.yml)

UGLY is a C library for calling between programming languages **inside one
process**. It embeds the real interpreters (Lua, CPython, QuickJS,
Perl 5, the JVM and the CLR) and routes every cross-language call through a
small, language-neutral C core.

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

-- Lua's own library takes Python functions.
print((string.gsub("hello world", "%w+", python("builtins").str.capitalize)))   --> Hello World

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

### In HAProxy

HAProxy has Lua built in, and the Lua binding attaches to it, so a Lua
action can ask Python whether to let a request through
([examples/haproxy/](examples/haproxy/), run by the tests when `haproxy` is
installed):

```lua
-- poc.lua, loaded with lua-load-per-thread
local ugly = require "ugly"
local policy

if core.thread == 0 then             -- lua-load: one state, run by every thread in turn
    ugly.shared_by_threads()
end

core.register_action("decide", { "http-req" }, function(txn)
    local ok, verdict = pcall(function()
        policy = policy or ugly.module("python", "policy")    -- Python starts on the first request
        return policy.decide(txn.sf:src(), txn.sf:path(), txn.sf:req_hdr("user-agent"))
    end)
    txn:set_var("txn.verdict", ok and verdict[1] or "deny")    -- fail closed
end)
```

`policy.py` is plain Python. Both of HAProxy's Lua modes work with several
threads, all sharing the one Python: `lua-load-per-thread`, where each worker
thread has its own Lua state, and `lua-load`, where every thread takes its
turn in one state (HAProxy serializes it; the script declares it with
`ugly.shared_by_threads()`). Use `ugly` inside actions, not when the file
loads: HAProxy loads Lua on its main thread, and with `lua-load-per-thread`
the state that attached there would belong to the wrong thread. (With
`lua-load`, Python can be started when the file loads, in HAProxy's master:
the worker it forks inherits a working interpreter, which the tests check
with `-W`.) The Python call runs synchronously in the action, so it
suits quick decisions, not I/O.

### From Python

The other way round works the same, with `import ugly` in a plain `python3`
program. These lines are checked by
[tests/python/test_binding.py](tests/python/test_binding.py):

```python
import ugly

string = ugly.module("lua", "string")             # a Lua module
string.upper("abc")                               # 'ABC'
sorted(["banana", "Apple", "cherry"], key=string.lower)   # Python calls a Lua function
string.gsub("hello world", "%w+", str.capitalize) # 'Hello World': Lua calls a Python function

t = ugly.module("lua", "os").date("!*t", 0)       # a Lua table
t.year, t.month                                   # (1970, 1)

squares = ugly.eval("lua", "local t = {} for i = 1, 4 do t[i] = i * i end return t")
list(squares)                                     # [1, 4, 9, 16]
```

Code reached this way can use `ugly` too, and it reaches back into the
host's own interpreter. For example, Lua called from that Python program
can `require "ugly"` and use the program's own globals.

### A Haskell library

Haskell is compiled, so a module is a library built for UGLY: an export
module lists what to export, and Template Haskell generates the rest,
expanding polymorphic functions into one instantiation per instance.
[examples/haskell/](examples/haskell/) does it for
[music-diatonic](https://hackage.haskell.org/package/music-diatonic), and
uses it from Python ([music.py](examples/haskell/music.py)) and from Lua
([music.lua](examples/haskell/music.lua)):

```python
md = ugly.module("haskell", "Music.Diatonic")
show = md.show                                    # Haskell's show
[show(n) for n in md.notes(md.majorScale(md.D))]  # ['D', 'E', 'F#', 'G', 'A', 'B', 'C#']
show(md.tonic(md.minorScale(md.A)))               # 'A': chosen by the argument's type
show(md.majorChord(md.C).transpose(md.Min3rd, md.above))   # 'Eb': the receiver goes last
md.noteMap(lambda n: md.above(md.Perf5th, n), md.majorScale(md.D))   # a Python function as a Note -> Note
show(md.read("Am7", ugly.opts(hint="Chord")))     # 'Am7': a hint is a type application
```

The other way round, a Haskell program uses the other languages with
`Ugly.withUgly` ([tests/haskell/TestBinding.hs](tests/haskell/TestBinding.hs)):

```haskell
main = Ugly.withUgly $ do
  json <- Ugly.module_ "python" "json"
  xs <- Ugly.invoke json "loads" [Ugly.toValue "[1, 2, 3]"] >>= Ugly.fromValue
  print (sum xs :: Int)                             -- 6: a Python list as a [Int]
```

The plan, and how Haskell maps onto the contract, is
[doc/haskell-bridge.md](doc/haskell-bridge.md).

A QuickJS program (`qjs`) works the same way, by importing the native module
([tests/js/test_binding.js](tests/js/test_binding.js)):

```js
const ugly = await import("/path/to/ugly.so");
const functools = ugly.module("python", "functools");
functools.reduce((a, b) => a * b, [1, 2, 3, 4, 5]);   // 120: Python calls a JS arrow function
const date = ugly.module("python", "datetime").date(2026, 11, 25);   // a Python object
date.isoformat(); date.year;                      // "2026-11-25", 2026
ugly.module("lua", "string").upper("abc");        // "ABC"
```

A plain `perl` program binds modules of other languages to Perl packages,
and foreign lists and dicts behave as Perl arrays and hashes
([tests/perl5/test_binding.pl](tests/perl5/test_binding.pl)):

```perl
use Ugly python => [ 'math', 'json' ], java => 'java.util.ArrayList' ;

math::sqrt(16) ;                                  # 4
my $l = json::loads('[1, 2, 3]') ;                # a Python list
push @$l, 4 ;  shift @$l ;  splice(@$l, 0, 1, 'x') ;   # Python sees ['x', 3, 4]
my $al = java::util::ArrayList->new ;             # classes are packages with new()
my $d = json::loads('{"k": 1}') ;                 # a Python dict
delete $d->{k} ;  exists $d->{k} ;                # false
```

A plain `java` program uses `ugly.Ugly`, with `ugly.jar` on the class path
and `libugly_jni.so` on `java.library.path`
([tests/java/test_binding.java](tests/java/test_binding.java)):

```java
import java.util.List;
import java.util.function.BinaryOperator;
import ugly.Foreign;
import ugly.Ugly;

Foreign math = (Foreign)Ugly.module("python", "math");
math.invoke("sqrt", 16.0);                        // 4.0

Foreign functools = (Foreign)Ugly.module("python", "functools");
BinaryOperator<Object> times = (a, b) -> (Long)a * (Long)b;
functools.invoke("reduce", times, List.of(1L, 2L, 3L, 4L, 5L));   // 120: Python calls a Java lambda

Foreign date = (Foreign)((Foreign)Ugly.module("python", "datetime")).invoke("date", 2026, 11, 25);
date.invoke("isoformat");                         // "2026-11-25"

Foreign string = (Foreign)Ugly.module("lua", "string");
string.invoke("format", "%d items at %.2f", 3, 9.5);   // "3 items at 9.50"
```

```sh
java -cp ugly.jar:. -Djava.library.path=<libdir> Main
```

A .NET program references `Ugly.dll`, and `dynamic` makes foreign values
feel native; named arguments are keyword arguments
([tests/dotnet/binding/test_binding.cs](tests/dotnet/binding/test_binding.cs)):

```csharp
using Ugly;

dynamic json = Interop.Module("python", "json");
dynamic data = json.loads("{\"scores\": [3, 1, 4]}");
json.dumps(data, indent: 2);                              // Python's keyword argument, as a C# named argument

var functools = Interop.Module("python", "functools");
Func<object?, object?, object?> times = (a, b) => (long)a! * (long)b!;
functools.Invoke("reduce", times, new List<object> { 1L, 2L, 3L });   // 6: Python calls a C# lambda

Interop.Shutdown();                                       // teardown, on this thread
```

### Keyword arguments

Calls take keyword arguments where the callee's language has them: Python
gets them as keyword arguments, and Perl as trailing `name => value` pairs.
Python passes them natively; the other bindings mark them with `kw`, passed
last (all checked by the binding tests):

```python
perl.kw(1, c=3, b=2)                              # Python, into Perl: kw(1, 'c', 3, 'b', 2)
```
```lua
builtins.int("ff", ugly.kw{ base = 16 })          -- Lua: 255
```
```js
builtins.int("ff", ugly.kw({ base: 16 }));        // JS: 255
```
```perl
json::dumps($data, Ugly::kw(separators => $pair));   # Perl
```
```java
builtins.invoke("int", "ff", Ugly.kw("base", 16));   // Java: 255
```

Two more call options go the same way, with `opts`: a Java overload hint,
and Perl's call context:

```python
String = ugly.module("java", "java.lang.String")
String.valueOf(65, ugly.opts(hint="double"))      # '65.0': valueOf(double), not valueOf(long)

localtime = ugly.eval("perl5", "sub { localtime($_[0]) }")
localtime(0, ugly.opts(context="scalar"))         # a date string, not Perl's 9-element list
```

Lua, JS and Java have no keyword arguments, so calls into them refuse them
with an error rather than drop them. Calling a class that can only be
instantiated (a Perl package, a Java class) constructs it, so
`perl.KwBox(value=5)` works from Python as it would for a Python class.

### Configuring languages

Languages start on first use, so their settings are given beforehand, with
`configure` in any binding (or `ugly_configure` in C). Options are module
paths for every language, `cpath` for Lua, and the class path and JVM
options for Java. They come before the environment variables that do the
same (`PYTHONPATH`, `LUA_PATH`...):

```python
ugly.configure("lua", path="./lua/?.lua")         # Lua modules from ./lua, ahead of LUA_PATH
ugly.configure("java", options=["-Xmx64m"])       # JVM options (and classpath=[...])
ugly.module("lua", "greet").hello("you")          # finds ./lua/greet.lua
Runtime = ugly.module("java", "java.lang.Runtime")   # the JVM starts here, with its options
Runtime.getRuntime().maxMemory()                  # 67108864
```

Once a language has started, configuring it raises an error instead of
being silently ignored.

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

## Languages

Each language has a guide: using UGLY from it, how its values look to the
others, and its pitfalls.

| Language | Guide | Can be the host |
|---|---|---|
| Lua 5.4 or 5.3 | [doc/languages/lua.md](doc/languages/lua.md) | yes: `lua`, HAProxy, any program with Lua built in |
| CPython 3.10+ | [doc/languages/python.md](doc/languages/python.md) | yes: `python3` |
| JavaScript (QuickJS) | [doc/languages/js.md](doc/languages/js.md) | yes: `qjs` |
| Perl 5 | [doc/languages/perl5.md](doc/languages/perl5.md) | yes: `perl` |
| Java (JDK 21+) | [doc/languages/java.md](doc/languages/java.md) | yes: `java` |
| C# / .NET 10 | [doc/languages/dotnet.md](doc/languages/dotnet.md) | yes: any .NET program |
| Haskell (GHC 8.8+) | [doc/haskell-bridge.md](doc/haskell-bridge.md) (the plan, until its guide) | yes: a program built with `-package ugly` (`Ugly.withUgly`) |

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
- **Call options** carry what isn't an argument: keyword arguments, an
  overload hint (Java, C#), Perl's call context. The callee's language honours each,
  or refuses the call; none is silently dropped.
- **Threads**: a runtime belongs to the thread that created it, except
  where its language can serve every thread (Python, through the GIL, the
  JVM and the CLR): those are shared, so each thread of a host like HAProxy can
  have its own Lua and still reach the one Python in the process. Threads
  that Python or Java start get a context of their own automatically.

## Tradeoffs

Sharing one process is the point, and it has a price.

- **No isolation.** Every runtime shares the address space, so a crash in
  one (a segfault in an extension module, the JVM running out of memory)
  takes the host down, and nothing stops one language's code from doing
  anything the process can. UGLY is for code you trust. Signals belong to
  whoever installed them first: Python is started without its handlers,
  the JVM leaves the shutdown signals to the host (`-Xrs`), the plugin
  puts back the host's after the CLR starts, and a plugin restores any
  handler its interpreter resets while starting. The JVM and the CLR keep
  the fault handlers they need, and pass on a fault outside their own code
  to the handler installed before them, so the two work in either order.
- **Calls are synchronous.** A call into another language is a plain C
  call: the caller waits, and Lua can't yield across it. That suits quick
  work (lookups, scoring, validation, reusing parsing or policy code). Code
  that does I/O belongs behind the host's asynchronous mechanisms, or out
  of process. With several threads, the GIL serializes the Python work (and
  makes each contended call cost microseconds: see Performance), and in
  HAProxy's `lua-load` mode every thread waits for the one Lua state.
- **Cycles across runtimes leak.** A Perl object holding a Lua proxy whose
  table holds that Perl object is invisible to both collectors (contract
  §3). A single VM such as GraalVM collects it; UGLY doesn't.
- **Builds are local.** The Python and Perl plugins are tied to the exact
  interpreter they're built against (see [Releases](#releases)).

If you need isolation, a sandbox or untrusted code, the Wasm Component
Model or an out-of-process bridge (gRPC, HAProxy's SPOE) is the better
fit; if you need cross-language inlining in hot loops, GraalVM is. The
[design essay](doc/design-essay.md) compares them.

## Performance

[doc/benchmarks.md](doc/benchmarks.md) has the numbers, from
`meson test -C build --benchmark`, and how to read them. In short, on a
laptop (i7-1365U):

- A call from C into Lua, Python or JS costs 60 to 120 ns; into Perl or
  .NET 120 to 160 ns; into Java or Haskell about 400 ns. Between two
  languages, 100 to 500 ns, and up to 900 ns into Java or Haskell.
- Calls into separate runtimes scale with threads: 63 million calls per
  second on 6 threads into C callbacks, 35 million into a Lua runtime per
  thread.
- Python doesn't: every call takes the GIL, and several threads calling it
  at once make each call cost microseconds.

## Layout

| Path | What |
|---|---|
| `include/ugly.h` | The public API. |
| `include/ugly_plugin.h` | The plugin contract: what a language plugin implements, and the core services it may use. |
| `src/` | The core (`libugly`): handle table, call frames and borrows, errors, threads, plugin loading. Also the built-in "c" language, which is the home of C callbacks and core lists and maps. |
| `plugins/mock/` | A small language written in C, used to test the core and the contract before any real interpreter. |
| `plugins/lua/` | Lua 5.4 or 5.3, chosen at build time (`-Dlua_version`); one Lua per process. `binding/<version>/` builds the same source as the Lua module `ugly`, for every Lua found. |
| `plugins/python/` | CPython 3.10+. One runtime per process; Python is never finalized. `binding/` builds the same source as the Python extension `ugly`. |
| `plugins/js/` | JavaScript on QuickJS, built from a pinned release (`subprojects/quickjs.wrap`). ES modules are loaded from `UGLY_JS_PATH`. `binding/` builds the same source as the native module `ugly.so` for `qjs`. |
| `plugins/perl5/` | Perl 5. Modules and classes are packages; calls run in list context, unless a call asks for another. Its Perl side (proxies, ties, `use Ugly LANG => MODULE`) is `helper.pm`, embedded at build time. `binding/` builds the same source as the XS module `Ugly` (`use Ugly`). |
| `plugins/java/` | Java through JNI. One JVM per process; classes are imported by name and their overloads chosen by how well the arguments convert. The helper classes (`ugly.Foreign`, `ugly.ForeignException`, `ugly.Kw`, `ugly.Opts`, `ugly.Bridge`) are `ugly.jar`. `binding/` builds the same source as `libugly_jni.so`, which `ugly.Foreign` loads in a plain `java` program. |
| `plugins/dotnet/` | C# and .NET, on the CLR through its hosting API (`hostfxr`). One CLR per process; types are imported by full name from loaded assemblies or the module path, overloads are chosen as in Java, and keyword arguments bind to parameters by name. Most of it is C# (`Ugly.dll`), which calls the core through P/Invoke; `Ugly.Foreign` is a `DynamicObject`. `Ugly.dll` is also the binding: a .NET program that references it uses the other languages through `Ugly.Interop`. |
| `tests/` | The conformance suite: the contract, as tests. Each plugin's fixtures are in `tests/<lang>/`. |
| `examples/` | Example programs, run as tests: Lua using Python, a C host, HAProxy asking Python for a decision on each request (`haproxy/`), and a Haskell library used from Python and Lua (`haskell/`). |
| `doc/` | The contract, the design essay, a guide per language (`doc/languages/`), the benchmarks, and a guide to writing a plugin (`plugin-guide.md`). |

## Status

Lua (5.4 or 5.3), CPython, JavaScript (QuickJS), Perl 5, Java, C# and
Haskell pass the whole conformance suite, and every pair of them works in
one process. Each one can also be the host (Haskell: a program built
against the package `ugly`): plain `lua`, `python3`, `qjs`, `perl`, `java`
and .NET programs use the other languages, and so does HAProxy's built-in Lua, on
several threads. Everything [doc/contract.md](doc/contract.md) specifies is
built, and the current release is on the Releases page.

Open work (LuaJIT, iterators, more languages, packages) is in
[TODO.md](TODO.md).

History is in the tags. `v0.01` is the original 2013–2014 code, including an
early Java plugin and a JNA binding. `v0.02` is that first design brought
up to date: a Perl 5 plugin and an XS binding, with an autotools-free build
and a test suite. Neither is in the tree any more. Java came back in v2
through JNI, which JDK 21 still needs; the Panama FFM API is final from
JDK 22.

## Releases

Releases are source tarballs, on the
[GitHub Releases](https://github.com/patrickleboutillier/ugly/releases)
page with their checksums. Each one was built and tested by CI from the
tarball itself (`meson dist`), and includes QuickJS, so it builds without a
download. There are no binaries: the Python and Perl plugins are tied to the
exact interpreter they're built against, so they're built where they're
used.

## Building and testing

You need a C toolchain, Meson (0.61 or later) and Ninja. Each language
plugin, and its binding, is built only if its language is found: Lua 5.4 or
5.3 (`liblua5.4-dev`, `liblua5.3-dev`), CPython 3.10+ (`python3-dev`), Perl 5 (`libperl-dev`)
a JDK, 21 or later (`openjdk-21-jdk-headless`, found through `javac`),
a .NET 10 SDK (`dotnet-sdk-10.0`, found through `dotnet`), and GHC 8.8 or
later with shared libraries (`ghc`). The Haskell example downloads
music-diatonic from Hackage at setup, checksummed like QuickJS.
QuickJS is built from source: Meson downloads its pinned release at setup,
and checks the checksum. `-Dlua=disabled`, `-Dpython=disabled`,
`-Djs=disabled`, `-Dperl5=disabled`, `-Djava=disabled`, `-Ddotnet=disabled`
and `-Dhaskell=disabled` skip a language;
`--wrap-mode=nodownload` forbids the download. The plugin uses Lua 5.4 if
it's there, else 5.3 (`-Dlua_version=5.3` chooses), and the Lua binding is
built for each; its tests need the matching interpreters (`lua5.4`,
`lua5.3`). Valgrind is optional.

```sh
meson setup build
meson compile -C build
meson test -C build        # the conformance suite, pairwise tests and examples
```

The tests need no install step. `meson install -C build` installs the
library, the two headers, a pkg-config file, the plugins and the bindings
(the Lua module where each Lua version looks for C modules, the Python
extension in site-packages, the Java one in libdir), and the Haskell
package to build module libraries against (`ghc -package-db
<libdir>/ugly/haskell/package.db -package ugly`). Plugins are looked up in `<libdir>/ugly/plugins/`, and
`UGLY_PLUGIN_PATH` (a colon-separated list of directories) overrides that at
runtime.

### Tests

Every test is a TAP stream. The full output is in
`build/meson-logs/testlog.txt`.

- **Conformance** (`tests/*.c`): nine groups, which are values, protocol,
  ownership, identity, callbacks, errors, threads, lifecycle and call
  options. Each group
  runs against every plugin built, as suite `conformance-<plugin>`. The
  fixtures are each plugin's `conformance` module, from `tests/<lang>/`.
- **Pairwise** (`tests/pairwise.c`): every ordered pair of different
  languages in one process, using each other's values, calls and errors.
- **Bindings** (`tests/<lang>/test_binding.*`): plain `python3`, `lua5.4`,
  `lua5.3`, `qjs`, `perl`, `java` and `dotnet` programs using the other languages
  through `ugly`, in attach mode. The `qjs` is the one built from the
  QuickJS subproject.
- **Examples** (`examples/`): run, and checked by exit status. They're the
  programs README quotes. The HAProxy ones run when `haproxy` is installed,
  against the Lua binding that matches its Lua.

```sh
meson test -C build --suite conformance-lua   # one plugin
meson test -C build --suite pairwise          # every pair of languages
meson test -C build --setup valgrind          # everything under valgrind
meson test -C build --setup helgrind threads-lua threads-python   # data races
```

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs all of it on
Ubuntu 22.04 and 24.04, the plugin on Lua 5.3, and builds with each
language left out.

The valgrind setup fails a test on any memory error and on any definite,
indirect or possible leak, so the tests delete everything they create.
Memory that is still reachable at exit is allowed, because a plugin may keep
process-wide state for the life of the process. `tests/valgrind.supp`
suppresses only CPython's own memory: its live objects reported as
"possibly lost" (from `libpython`, or from a statically linked `python3`
host), because Python is never finalized, and the interned strings Python
3.12 never frees when it is the host. Tests with a JVM or a CLR in them
(suites `jvm` and `clr`) don't run under valgrind: their JITs and GCs are
beyond what memcheck can judge. They run with `-Xcheck:jni` instead (`UGLY_JAVA_CHECK`), where a JNI
warning aborts. The HAProxy tests (suite `haproxy`) are left out too:
valgrind would wrap their runner, not HAProxy.

## License

Apache License 2.0. See [LICENSE](LICENSE).

Releases up to 2.4.0, and the `v0.01` and `v0.02` tags, were published under
the GPL v3, and stay under it; later versions are Apache-2.0.
