# C# and .NET

C# (and any .NET language) on the CLR, started through its hosting API
(`hostfxr`), language `"dotnet"`. It needs a .NET 10 SDK to build, and the
.NET 10 runtime to run. How .NET maps onto the protocol is the C# column of
[the contract's §10](../contract.md#10-the-languages); this page is the
practical side.

## From C#: the binding

A .NET program references `Ugly.dll` (from the plugin directory, or
`build/plugins/dotnet/`) like any assembly; that's the whole binding. It
finds libugly by itself: `UGLY_LIBRARY` (a path), else `libugly.so.2`
through the system's search (`LD_LIBRARY_PATH`, the installed libdir), else
beside `Ugly.dll`. The other languages' plugins come from `UGLY_PLUGIN_PATH`
or the installed plugin directory.

```xml
<ItemGroup>
  <Reference Include="Ugly"><HintPath>/usr/local/lib/x86_64-linux-gnu/ugly/plugins/Ugly.dll</HintPath></Reference>
</ItemGroup>
```

```csharp
using Ugly;

dynamic json = Interop.Module("python", "json");             // a module of another language
dynamic data = json.loads("{\"scores\": [3, 1, 4]}");
Console.WriteLine(data["scores"][0]);                          // 3
Console.WriteLine(json.dumps(data, indent: 2));               // named arguments are keyword arguments

var functools = Interop.Module("python", "functools");      // a Foreign
Func<object?, object?, object?> times = (a, b) => (long)a! * (long)b!;
functools.Invoke("reduce", times, new List<object> { 1L, 2L, 3L });   // 6: Python calls a C# lambda

Interop.Shutdown();                                           // on this thread, before the program ends
```

| `Ugly.Interop` | What it does |
|---|---|
| `Module(lang, name)` | imports a module of `lang` in this thread's runtime of it, as a `Foreign` |
| `Eval(lang, code)` | evaluates code in a language that can (`UGLY_CAP_EVAL`): a `Foreign`, or a plain value |
| `Configure(lang, key, values...)` | options for a language that hasn't started ([Configuring](#configuring)) |
| `Kw("name", value, ...)`, `Kw(dictionary)` | keyword arguments, passed last (or named arguments, through `dynamic`) |
| `Opts(hint: ..., context: ...)` | other call options, passed last (beside a `Kw`, in either order) |
| `IsForeign(x)` | whether a value is foreign |
| `Shutdown()` | tears UGLY down: see below |

**Iteration** (contract §2): a `Foreign` is an `IEnumerable<object?>`, so
`foreach` and LINQ run the foreign language's own iteration, one value per
step: a Python generator runs only as far as the loop goes, and `break` or
`Take(n)` on an infinite one is fine. A Lua iterator function works as is
(`string.gmatch`). A value its language doesn't iterate but that has
elements (a Lua table) is walked by index. A `Foreign` passed where a
method takes an `IEnumerable<T>` is its iteration, each element converted
as it comes. **`ToString()`** is the foreign language's string (Python's
`str()`, Lua's `tostring`).

```csharp
var itertools = Interop.Module("python", "itertools");
foreach (object? x in (Foreign)itertools.Invoke("count", 1)!) {   // an infinite Python iterator
    if ((long)x! > 3) break;
    Console.WriteLine(x);                                         // 1, 2, 3
}
```

The binding attaches on the first use of `Interop`, on that thread. Code in
other languages that asks for `"dotnet"` (`ugly.module("dotnet", ...)` in
Python) reaches this program's own CLR.

**Teardown is explicit.** Call `Interop.Shutdown()` on the thread that first
used UGLY: it deletes this program's runtime and the other languages'
runtimes that thread got, so their finalizers run (Lua's `__gc`, Perl's
`DESTROY`, JS's finalizers). .NET runs `ProcessExit` on another thread,
which can't do it. Without `Shutdown`, the process just ends.

## Using .NET from another language

```python
import ugly

Math = ugly.module("dotnet", "System.Math")        # a static class: a module
Math.Abs(-3)                                        # 3
Math.Round(2.5)                                     # 2.0: C#'s overloads, C#'s rules

sb = ugly.module("dotnet", "System.Text.StringBuilder")()   # a class: called, it constructs
sb.Append("a").Append(1)
sb.ToString()                                       # 'a1'
```

Modules are types, imported by full name: from an assembly already loaded,
then from `<longest prefix of the name>.dll` on the module path or among
the framework's assemblies, then from any assembly on the module path.

- A static class is a `module`; a type with a public constructor is a
  `class` (INSTANTIABLE). Public nested types are members.
- Members are public fields, properties and methods, or a dictionary's
  string keys (`IDictionary`, `ExpandoObject`). Elements are arrays and
  `IList`s.
- `invoke` calls an instance method on the object, or a static method of a
  type. A member that holds a delegate is called.

## Your own C# code

Build a class library against `net10.0`, and put its directory on the
module path:

```csharp
public static class Greeter {
    public static string Hello(string name, string greeting = "Hello") => $"{greeting}, {name}";
    public static object? Apply(Func<object?, object?> f, object? x) => f(x);
}
```

```python
ugly.configure("dotnet", path=["./bin"])           # before .NET starts
g = ugly.module("dotnet", "Greeter")
g.Hello("you", greeting="Hi")                       # 'Hi, you': keyword arguments are named arguments
g.Apply(lambda x: x * 2, 21)                        # 42: a Python function as a Func<object?, object?>
```

C# code that receives foreign values sees them as `Ugly.Foreign`
(reference `Ugly.dll`, next to the plugin, with `<Private>false</Private>`:
when another language started the CLR, it's already loaded). A `Foreign` is
a `DynamicObject`:

```csharp
using Ugly;

public static object? Dumps(object data) {
    dynamic json = Interop.Module("python", "json");
    return json.dumps(data, indent: 2);              // named arguments are keyword arguments
}
```

| `Ugly.Foreign` | |
|---|---|
| `Call(args...)`, `Invoke("m", args...)`, `New(args...)` | call it (a class that can only be instantiated constructs); invoke a method with its language's receiver rules; instantiate it |
| `Get("x")`, `Set("x", v)`, `this[i]`, `Length` | members; elements, 0-based |
| `Names`, `Traits` | member names; `UGLY_TRAIT_*` bits |
| through `dynamic` | `f.x`, `f.m(a, name: b)`, `f(a)`, `f[i]`, `f["k"]` |

`Interop` (above) is the rest of the API, embedded or not.

A foreign value passed for a delegate type (`Func<...>`, `Action<...>`,
your own) becomes a delegate that calls it, and for an interface a
`DispatchProxy` whose methods invoke the value's by name (its single
abstract method calls the value itself).

## .NET values elsewhere

| C# | goes out as | comes back as |
|---|---|---|
| `null` | NULL | `null` |
| `bool` | BOOL | `bool` |
| `long`, `int`, `short`, `byte`... ; `ulong`, `BigInteger`, `Int128` if they fit | INT | `long` |
| `double`, `float` | DOUBLE | `double` |
| `string`, `char` | STRING | `string` |
| `byte[]` | BYTES | `byte[]` |
| anything else (`decimal` too) | a handle | the original object |

- Iterable is what `foreach` takes: `IEnumerable` (arrays, lists,
  dictionaries, which yield `KeyValuePair`s, LINQ queries, `yield return`
  methods) and `IEnumerator`. `ugly_iter` calls `GetEnumerator()` (an
  enumerator's iterator is itself); each step is `MoveNext()`, so a
  `yield return` method runs only as far as asked. Releasing an enumerator
  that `ugly_iter` handed out calls its `Dispose()`, as a `break` out of
  `foreach` would, so a `yield return` method's `finally` runs. An
  enumerable that is its own enumerator (a `yield return` method's result,
  a LINQ query) gets an iterator of its own for each pass, so it can be
  looped over again, as in C#.
- `ugly_to_string` is `ToString()`.

## Calls and options

- **Overloads** are chosen as in Java, by scoring how well each argument
  converts: an exact type, then an integer narrowed to a parameter it
  fits, then a supertype, then a conversion to floating point (`double`,
  then `decimal`, then `float`, so `Math.Round(5)` is `Round(double)`),
  with `object` last. A tie fails with `UGLY_ERR_TYPE`.
- **Keyword arguments** bind to parameters by name, as C#'s named arguments
  do, and parameters left out keep their defaults. A name no overload has
  fails with `UGLY_ERR_TYPE`.
- **Hints** name the overload's parameter types, comma-separated, by C#
  alias or full name: `"int"`, `"int,string"`, `"System.Int32"`.
- Call contexts are refused.

## Errors

An error from another language is thrown into C# as `Ugly.ForeignException`,
with `ErrorType`, `Language`, `Message` and `Payload`. If it escapes C#
unhandled, it goes on unchanged. `throw new ForeignException(payload)`
rethrows a foreign exception object.

A C# exception going out has its type's name as its type, and is itself
the payload; it comes back to C# as the same object.

## Threads

The CLR serves every thread (`UGLY_CAP_ANY_THREAD`): there's one per process,
shared by every thread, and a native thread is attached to it on its first
call. Threads that C# starts get a context of their own when they call other
languages.

## Configuring

Key: `path` (directories of assemblies), ahead of `UGLY_DOTNET_PATH`.

## Pitfalls

- **`Interop.Shutdown()` belongs on the thread that first used UGLY**, and
  a program that never calls it skips the other languages' finalizers.
- **`dynamic` and `Foreign`'s own methods.** Through `dynamic`, `Foreign`'s
  own members win: a foreign member named `Call`, `Invoke`, `New`, `Get`,
  `Set`, `Length`, `Names`, `Traits`, `Equals` or `ToString` needs
  `f.Invoke("Call", ...)` or `f.Get("Length")`. Members are case-sensitive,
  so Python's `get` or Java's `size` are fine.
- **One CLR per process, for good.** The CLR can't be unloaded or started
  again, so configure first.
- **Type names, not namespaces.** `ugly.module("dotnet", "System.IO")` fails:
  import a type, `System.IO.File`. Generic type definitions aren't
  importable yet.
- **Numbers arrive as `long` and `double`.** Parameters narrow (`int`,
  `short`...) when the value fits; `object` parameters get the `long`.
  `decimal` stays a handle: no silent loss of precision.
- **Stack overflow can't be caught in .NET**, so the plugin checks the stack
  on every entry (`EnsureSufficientExecutionStack`): deep recursion fails
  with `InsufficientExecutionStackException`, cleanly.
- **Signals.** The CLR keeps the fault handlers it needs (SIGSEGV, SIGFPE,
  SIGILL, SIGBUS, SIGTRAP, and SIGRTMIN to suspend threads) and passes
  faults outside managed code on to the handlers installed before it; the
  plugin puts the host's SIGINT, SIGQUIT, SIGTERM, SIGPIPE and SIGABRT back
  after starting it. But C# code that uses `Console` or `Process` makes .NET
  install its own terminal and child-process handlers (SIGCHLD, SIGCONT,
  SIGWINCH...) at that point. Not tested yet.
- **Release follows the GC.** A `Foreign` releases its handle from its
  finalizer, so foreign objects live until a collection. That goes for a
  foreign iterator a `foreach` left early too: a Python generator is
  closed when its `Foreign` is collected, not at the `break`.
- **A `yield return` object handed to another language as an enumerable**
  is enumerated from its start each time that language loops over it
  (`GetEnumerator()`, as `foreach` does); `ugly_iter`'s enumerator is the
  one that goes on where it stopped.
- **Not under valgrind.** Tests with a CLR are suite `clr`, left out of the
  valgrind and helgrind setups.
