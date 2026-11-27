# Java

Java on the JVM, through JNI (JDK 21 or later), language `"java"`. How Java
maps onto the protocol is the Java column of [the contract's §10](../contract.md#10-the-languages);
this page is the practical side.

## From Java: the binding

A plain `java` program uses the classes in `ugly.jar`, and `ugly.Foreign`
loads the binding's native library, `libugly_jni.so` (no `libjvm`: the JVM
is yours), from `java.library.path`, or from `-Dugly.library=<path>`:

```sh
java -cp ugly.jar:. -Djava.library.path=<libdir> Main
```

```java
import java.util.List;
import java.util.function.BinaryOperator;
import ugly.Foreign;
import ugly.Ugly;

Foreign math = (Foreign)Ugly.module("python", "math");
math.invoke("sqrt", 16.0);                                // 4.0

Foreign functools = (Foreign)Ugly.module("python", "functools");
BinaryOperator<Object> times = (a, b) -> (Long)a * (Long)b;
functools.invoke("reduce", times, List.of(1L, 2L, 3L));   // 6: Python calls a Java lambda
```

| `ugly.Ugly` | What it does |
|---|---|
| `module(lang, name)` | imports a module of `lang` in this thread's runtime of it |
| `eval(lang, code)` | evaluates code in a language that can (`UGLY_CAP_EVAL`) |
| `configure(lang, key, values...)` | options for a language that hasn't started ([Configuring](#configuring)) |
| `kw("name", value, ...)`, `kw(map)` | keyword arguments, passed last |
| `opts().hint(...).context(...)` | other call options, passed last (beside a `kw`, in either order) |
| `isForeign(x)` | whether a value is foreign |

The binding attaches on the thread that loads `ugly.Foreign`, and tears
down when the JVM ends (JVMTI's VMDeath, on `main`'s thread).

## Foreign values in Java

A foreign value is an `ugly.Foreign`, one per value (`equals` is identity).
Java has no operator overloading, so it's all methods:

| `Foreign` | |
|---|---|
| `call(args...)`, `invoke("m", args...)` | call it; invoke a method with the foreign language's receiver rules |
| `get("x")`, `set("x", v)` | members |
| `get(i)`, `set(i, v)`, `length()` | elements, 0-based |
| `names()`, `traits()` | member names; `UGLY_TRAIT_*` bits |
| `iterator()`, for-each | the value's own iteration (`Foreign` is an `Iterable<Object>`) |
| `toString()` | its own language's string for it |

Results are `Object`: cast them. A foreign value passed to a Java method
that expects an interface becomes a dynamic proxy for it: its single
abstract method calls the value, and its other methods invoke the value's
by name. So a Lua or Python function works as a `Function<Object, Object>`.

**Iteration** (contract §2): `Foreign` is an `Iterable<Object>`, so
for-each, `forEach` and anything taking an `Iterable` run the foreign
language's own iteration, one value per step: a Python generator runs only
as far as the loop goes, and `break` on an infinite one is fine. A Lua
iterator function works as is (`string.gmatch`). A value its language
doesn't iterate but that has elements (a Lua table) is walked by index.
`toString()` is the foreign language's string (Python's `str()`).

```java
Foreign itertools = (Foreign)Ugly.module("python", "itertools");
for (Object x : (Foreign)itertools.invoke("count", 1)){     // an infinite Python iterator
	if ((Long)x > 3) break;
	System.out.println(x);                                    // 1, 2, 3
}
```

## Java values elsewhere

| Java | goes out as | comes back as |
|---|---|---|
| `null` | NULL | `null` |
| `Boolean` | BOOL | `Boolean` |
| `Long`, `Integer`, `Short`, `Byte`, `BigInteger` (if it fits) | INT | `Long` |
| `Double`, `Float` | DOUBLE | `Double` |
| `String`, `Character` | STRING | `String` |
| `byte[]` | BYTES | `byte[]` |
| anything else | a handle | the original object |

- Members are public fields and methods, then bean getters (`getX()`,
  `isX()` for `x`), or a `Map`'s keys. Elements are arrays and `List`s.
- `invoke` calls an instance method on the object, or a static method of a
  class.
- A class with a public constructor is a `class` (INSTANTIABLE); one without
  is a `module` of its static members. Public nested classes are members.
- Iterable is what for-each takes (`Iterable`, arrays), and what Java
  iterates besides: an `Iterator` (its own iterator), and streams
  (`BaseStream`). A `Map` isn't, as in Java. Each step is `hasNext()` and
  `next()`, only as many times as asked; a batch crosses JNI once per 256
  values. Releasing the iterator over a stream closes the stream
  (`onClose` handlers run), as try-with-resources would.
- `ugly_to_string` is `toString()`.

## Java as a guest

The plugin creates the JVM when it loads, with `-Xrs`, its class path being
`ugly.jar`, then the `classpath` option, `UGLY_JAVA_CLASSPATH` and
`CLASSPATH`. `ugly_rt_import` loads a class by its full name
(`java.util.ArrayList`). There's no `eval` and no globals.

**Overloads** are chosen by scoring how well each argument converts: an
exact type is best, then an integer narrowed to a parameter it fits, then a
supertype, then a conversion to floating point, with `Object` last. A tie
fails with `UGLY_ERR_TYPE` ("ambiguous call"). An overload hint names the
one to use, by the parameter types' `Class.getName()` names, comma-separated
(`"int,java.lang.String"`; `char[]` is `"[C"`). A callable that isn't
overloaded (a lambda, any functional interface) takes only the hint that
names its one method's parameters, `"java.lang.Object"` for a `Function`:
another is refused, never passed on to a call it makes.

```python
String = ugly.module("java", "java.lang.String")
String.valueOf(65)                                # '65': valueOf(long), the best fit
String.valueOf(65, ugly.opts(hint="double"))      # '65.0': valueOf(double)
```

Keyword arguments and call contexts are refused.

## Errors

An error from another language is thrown as `ugly.ForeignException`, with
`type()`, `language()`, `getMessage()` and `payload()`. If it escapes Java
unhandled, it goes on unchanged. `throw new ForeignException(payload)`
rethrows a foreign exception object you got as a payload.

A Java exception going out has its simple class name as its type, and is
itself the payload; it comes back to Java as the same `Throwable`.

## Threads

The JVM serves every thread (`UGLY_CAP_ANY_THREAD`): there's one, shared by
every thread that uses it. Java threads that call other languages get a
context of their own automatically, deleted when the thread ends.

## Configuring

Keys: `classpath` and `options` (JVM options such as `-Xmx512m` or
`-Dkey=value`), ahead of `UGLY_JAVA_CLASSPATH` and `CLASSPATH`:

```python
ugly.configure("java", classpath=["lib/a.jar"], options=["-Xmx64m"])
```

## Pitfalls

- **One JVM per process, for good.** The JVM can't be created twice, or
  again after `DestroyJavaVM`. Its class path and options are fixed when it
  starts, so configure them first; configuring later raises an error.
- **Numbers arrive as `Long` and `Double`.** Cast, or use `Number`.
  Arguments narrow to `int`, `short`... parameters only when the value fits.
- **Releases follow the GC.** A `Foreign` releases its handle when it's
  collected (a `Cleaner`), so foreign objects live until the JVM's next
  collections. That goes for a foreign iterator a for-each left early too:
  a Python generator is closed when its `Foreign` is collected.
- **A stream is iterated once**, as in Java: a second `ugly_iter` on it is
  Java's `IllegalStateException`.
- **Overloads.** An INT argument is a `Long`, so `f(long)` beats `f(int)`,
  and both beat `f(double)` and `f(Object)`. When the choice matters, give
  a hint.
- **Arrays are fixed size.** Splicing a Java array fails with
  `UGLY_ERR_NOT_CAPABLE`; `List`s resize.
- **Diagnostics go to stderr.** The JVM's own output is sent to stderr. Under
  `-Xcheck:jni`, the JVM warns at exit ("handler modified") when another
  runtime installed signal handlers after it. The CLR does, and passes
  faults outside managed code on to the JVM's handlers, so Java's null and
  divide checks keep working.
- **Not under valgrind.** The JVM's JIT and GC confuse memcheck; tests with
  a JVM are suite `jvm`, left out of the valgrind setup.
