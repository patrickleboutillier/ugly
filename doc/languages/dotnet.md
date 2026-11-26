# C# and .NET

C# (and any .NET language) on the CLR, started through its hosting API
(`hostfxr`), language `"dotnet"`. It needs a .NET 10 SDK to build, and the
.NET 10 runtime to run. How .NET maps onto the protocol is the C# column of
[the contract's §10](../contract.md#10-the-languages); this page is the
practical side.

## No binding yet

C# can't host UGLY yet: a .NET program can't `using Ugly` and import Python.
The plugin is there, so other languages (and C) use .NET, and C# code they
call can use the other languages. The binding is in
[TODO.md](../../TODO.md).

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
it's already loaded). A `Foreign` is a `DynamicObject`:

```csharp
using Ugly;

public static object? Dumps(object data) {
    dynamic json = Interop.Module("python", "json");
    return json.dumps(data, indent: 2);              // named arguments are keyword arguments
}
```

| `Ugly.Foreign` | |
|---|---|
| `Call(args...)`, `Invoke("m", args...)`, `New(args...)` | call it; invoke a method with its language's receiver rules; instantiate it |
| `Get("x")`, `Set("x", v)`, `this[i]`, `Length` | members; elements, 0-based |
| `Names`, `Traits` | member names; `UGLY_TRAIT_*` bits |
| through `dynamic` | `f.x`, `f.m(a, name: b)`, `f(a)`, `f[i]`, `f["k"]` |

`Interop.Module(lang, name)`, `Interop.Eval(lang, code)` and
`Interop.IsForeign(x)` are the rest of the API.

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

## Calls and options

- **Overloads** are chosen as in Java, by scoring how well each argument
  converts: an exact type, then an integer narrowed to a parameter it
  fits, then a supertype, then a conversion to floating point, with
  `object` last. A tie fails with `UGLY_ERR_TYPE`.
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
  finalizer, so foreign objects live until a collection.
- **Not under valgrind.** Tests with a CLR are suite `clr`, left out of the
  valgrind and helgrind setups.
