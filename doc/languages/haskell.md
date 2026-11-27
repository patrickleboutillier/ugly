# Haskell

Haskell compiled by GHC (8.8 or later, with shared libraries and the
threaded RTS: Debian's `ghc`), language `"haskell"`. GHC's runtime system
runs in the host's process. How Haskell maps onto the protocol is the
Haskell column of [the contract's §10](../contract.md#10-the-languages);
this page is the practical side, and ends with the
[design choices](#design-choices) that shaped it.

Haskell is compiled, and has no reflection at run time, so it differs from
the other languages in one way: **a module is a library built for UGLY**.
Its export list is written by hand, and Template Haskell generates the
rest at compile time. A polymorphic function becomes one instantiation per
instance in scope, and a call picks one by its arguments' types.

## From Haskell: the binding

The binding is the Haskell package `ugly` itself. A program built against
it uses the other languages inside `Ugly.withUgly`:

```haskell
{-# LANGUAGE OverloadedStrings #-}
import qualified Ugly

main :: IO ()
main = Ugly.withUgly $ do
  json <- Ugly.module_ "python" "json"
  xs <- Ugly.invoke json "loads" ["[1, 2, 3]"]
  print (sum (xs :: [Int]))                     -- 6: a Python list, as a [Int]
  twice <- Ugly.eval "python" "lambda f, x: f(f(x))"
  r <- Ugly.callValue twice [Ugly.toValue ((+ 1) :: Int -> Int), 40]
  print (r :: Int)                              -- 42: Python calls a Haskell function
```

Build it dynamically, with the threaded RTS, against the installed package
(`meson install` puts it in `<libdir>/ugly/haskell/`; in a build tree it's
`build/plugins/haskell/package.db`):

```sh
ghc -dynamic -threaded -package-db /usr/local/lib/x86_64-linux-gnu/ugly/haskell/package.db -package ugly Main.hs
```

| `Ugly` | What it does |
|---|---|
| `withUgly act` | runs `act` with UGLY attached to this program's runtime, and detaches after (nesting is fine) |
| `module_ lang name` | a module of another language, as a `Foreign` |
| `eval lang code` | code evaluated in a language that can (`UGLY_CAP_EVAL`) |
| `call f args`, `invoke f "m" args` | call a foreign value; invoke its method with its language's receiver rules |
| `call_`, `invoke_`, `callValue_` | the same, dropping the result (a call made for its effect) |
| `get f "x"`, `index f i`, `size f`, `members f`, `typeOf f` | members; elements, 0-based; their count; member names; its type's name |
| `callValue v args` | call any callable `Value`, Haskell's own or foreign |
| `toValue`, `fromValue` | a Haskell value as a `Value` (`ToUgly`), and a `Value` you hold as a Haskell type (`FromUgly`) |
| `rethrow`, `exceptionValue` | raise a `Value` again (an exception, or a foreign value as an error's payload); an exception as a `Value` |

**Results come back as the type the caller wants**: `call`, `invoke`,
`get`, `index`, `callValue` and `eval` return any `FromUgly` type, converted
as an argument would be (a Python list as a `[Int]`, a str as a `String`, a
foreign object as a `Foreign`); a refusal is a `TypeError`. Ask for a
`Value` to get the raw result: `VNull`, `VBool`, `VInt`, `VDouble`,
`VString` (`Text`), `VBytes`, `VBox` (a Haskell value) or `VForeign`. A
result nothing uses has no type to infer: use the `_` forms.

**Arguments are `Value`s.** With `OverloadedStrings`, string and number
literals are `Value`s (`"x"`, `3`, `-1.5`): `Value` has `IsString`, `Num` and
`Fractional` instances, whose arithmetic is there for literals (a negative
one is `negate 1`) and is an error on anything but a number. Anything else
goes through `toValue`: a variable, a function, a list.

Each operation runs on the calling thread, through the core.

A program's own export modules (`uglyModule` in the program, below) are
found in the process, with no library, when it's linked with `-rdynamic`:
Python's `ugly.module("haskell", "Main")` reaches them.

## Your own Haskell code: module libraries

An export module lists what other languages see. `uglyModule` does the
rest (`Ugly.Export`):

```haskell
{-# LANGUAGE TemplateHaskell #-}
module Geometry (Point (..), norm, scale) where

import Ugly.Export (uglyModule)

data Point = Point { px :: Double, py :: Double }
  deriving Show

norm :: Point -> Double
norm (Point x y) = sqrt (x * x + y * y)

scale :: Double -> Point -> Point
scale k (Point x y) = Point (k * x) (k * y)

uglyModule "Geometry" [''Point, 'Point, 'norm, 'scale]
```

Build it into a **module library**, a `.so` with no RTS of its own, named
after the module, and put its directory on the module path:

```sh
ghc -dynamic -shared -fPIC -package-db <libdir>/ugly/haskell/package.db -package ugly \
    -this-unit-id geometry -outputdir obj -o lib/Geometry.so Geometry.hs
```

```python
ugly.configure("haskell", path="./lib")      # before Haskell starts
geo = ugly.module("haskell", "Geometry")
p = geo.Point(3, 4)                           # a constructor (new(Point, 3, 4) too)
p.px, geo.norm(p)                             # (3.0, 5.0): a record's fields are members
geo.norm(p.scale(2))                          # 10.0: methods take the receiver last
```

- **Listed types** (`''Point`) get `ToUgly` and `FromUgly` instances unless
  they have them: a box whose text is its `show` (with `Show`), whose
  methods are the module's functions taking it **last** (`p.scale(2)` is
  `scale 2 p`), and, for a record with one constructor, whose fields are
  members. Types of kind `*` only.
- **Listed values** (`'norm`, a constructor `'Point`, a class method) are
  members. A data constructor is also what `new` calls. A polymorphic one
  is expanded into its instantiations, over the instances in scope where
  the export module is compiled; those whose types can't cross are left
  out, and a value with none left is a compile error.
- **One library can hold several modules.** `import "A.B.C"` looks for
  `A.B.C.so`, then `A.B.so`, then `A.so`, in each directory of the `path`
  option, then of `UGLY_HASKELL_PATH`.
- **A real library**: [examples/haskell/](../../examples/haskell/) exports
  [music-diatonic](https://hackage.haskell.org/package/music-diatonic) and
  uses it from Python and Lua.
- **Written by hand**, with `Ugly.Value`, when a box needs more than a type
  gives: `moduleBox` and `uglyModuleFrom "Name" 'box` for a module,
  `Members` and `Elems` with setters and a splice for a mutable value
  (`IORef`s behind a box), and `uglyToException`/`uglyFromException` for an
  exception that crosses with its own members. The conformance fixture,
  [tests/haskell/Conformance.hs](../../tests/haskell/Conformance.hs), has
  all three.

Foreign values reach an export as its parameters' types say. As a
`Foreign`, or a `Value` holding one, it's a handle the module may keep
(released when Haskell drops it). As `[a]`, a foreign sequence is
converted element by element when the call runs, and `[Value]` reads it
lazily. **As a function type** (`Note -> Note`), a foreign callable becomes
a Haskell function: one call with all its arguments, pure (called when its
result is forced) unless its result is in `IO`.

## Haskell values elsewhere

| Haskell | goes out as | comes back as |
|---|---|---|
| `()`, `Nothing` | NULL | `()`, `Nothing` |
| `Bool` | BOOL | `Bool` |
| `Int`, `Int8`..`Int64`, `Word8`..`Word32` | INT | any integer type it fits |
| `Integer`, `Natural`, `Word`, `Word64` | INT if it fits, else a box of its digits | the same |
| `Double`, `Float` | DOUBLE | `Double`, `Float` |
| `String`, `Text`, `Char` | STRING | `String`, `Text`, `Char` (one character) |
| `ByteString` | BYTES | `ByteString` |
| `Just x` | `x` (a box, if `x` is itself null) | |
| a list, a tuple | a handle: read-only elements | the original value |
| a `Map String v` | a handle: read-only members | the original value |
| a function, an `IO` action | a handle: callable | the original function |
| anything else | a handle: a type name, a text if it has `Show` | the original value |

- **Haskell values are immutable**: setting a member or an element is
  `UGLY_ERR_NOT_CAPABLE`, except on a mutable value a module makes itself.
- **Lists are lazy.** An element is computed when it's read, so `[0..]` has
  a thousandth element. Element *i* is *i* steps from the start.
- **Calls are curried**: fewer arguments than the arity give a partial
  application, itself callable. An `IO` action is called with no arguments.
- **`describe`** gives the type's name as `Typeable` shows it: `Note`,
  `[Int]`, `Map [Char] Int`.

## Haskell as a guest

`ugly_rt_import` and `ugly.module("haskell", name)` find a module library
(above). There's no `eval` and no `globals`: Haskell has neither at run
time. A runtime holds nothing yet: modules are loaded once per process,
and every runtime gets the same ones.

## Calls and options

An argument converts the way a literal of its kind would, at a cost that
ranks a polymorphic function's instantiations: an int is an `Int` (cost
0), an `Int64` or `Integer` (1), a narrower type it fits (2), a `Double`
(3); a string is a `String` (0), `Text` (1), a `Char` (2); a Haskell value
coming home is its own type (0); anything is a `Value` (4). An int that
doesn't fit is refused, never wrapped. A string is never `read`.

The cheapest instantiation is called. A tie is refused (`UGLY_ERR_TYPE`),
and the message lists each instantiation with the **hint** that picks it.
The hint is type arguments, as `TypeApplications` writes them:

```python
md.read("Am7", ugly.opts(hint="Chord"))          # read @Chord
md.tonic(scale, ugly.opts(hint="@Scale @Note"))   # tonic :: Deg s n => s -> n: both variables
md.tonic(scale, ugly.opts(hint="@_ @Note"))       # the second only; the first left open
```

**`want` ranks by result**, for a function polymorphic in its result alone:
`read("42")` wanting an int is `read @Int`, wanting a double `read
@Double`. A result that can't be the kind wanted is refused before the
call runs. Keyword arguments and a call context are refused
(`UGLY_ERR_NOT_SUPPORTED`): Haskell has neither.

## Errors

A Haskell exception going out has its type's name (`ErrorCall`,
`ArithException`), `displayException` as its message, and the exception as
its payload. Coming home, the payload is thrown again as itself, so
Haskell catches its own `ErrorCall` even after it went through Python.

A failure of an operation on a foreign value is a `ForeignError`, with
`errorLanguage`, `errorType`, `errorMessage`, `errorStatus` and
`errorPayload`:

```haskell
r <- try (Ugly.invoke_ json "loads" ["{"])
case r of
  Left e -> putStrLn (Ugly.errorLanguage e ++ " " ++ Ugly.errorType e)   -- python JSONDecodeError
  Right _ -> return ()
```

If it escapes Haskell, the original error goes on unchanged. A conversion
that's refused inside Haskell code (`fromValue`, a callback's result) is a
`TypeError`.

## Threads

Any thread can call into Haskell (`UGLY_CAP_ANY_THREAD`), and every call
from C runs as a bound thread on its caller's OS thread. Haskell code that
uses a runtime bound to a thread (Lua's) must call it from that thread: a
`forkIO` thread gets `UGLY_ERR_WRONG_THREAD`. Foreign values are released
by finalizers, on the RTS's finalizer thread; the core defers each release
to the value's home thread.

## Configuring

Keys: `rts`, RTS options, as between `+RTS` and `-RTS` (`-N4`; the RTS also
reads `GHCRTS`), and `path`, directories of module libraries, ahead of
`UGLY_HASKELL_PATH`. In the binding the RTS is the program's, so only
`path` applies.

## Design choices

Decided by the author (2026-11-26), on questions of idiom and feel:

1. **`invoke` puts the receiver last**, as Haskell puts the subject last
   (`x & f a b`): `chord.transpose(Min3rd, above)` is `transpose Min3rd
   above chord`, a function of the module that exported `Chord`. On a
   module, `invoke` calls its function with no receiver.
2. **Integers that don't fit are refused** (`UGLY_ERR_TYPE`), never wrapped
   as `fromInteger` would; this also rules out an instantiation whose
   integer type is too small. Going out, an `Integer` beyond int64 is a box
   of its digits.
3. **`Maybe` is flattened**: `Nothing` is null, `Just x` is `x`, unless `x`
   is itself null; then it stays a box one element long, so nothing is lost.
4. **Export lists are written by hand** (`uglyModule` with names). A tool
   that drafts one from the GHC API can come later.
5. **No cap on `length`** of a lazy list: an infinite one doesn't return,
   as in Haskell.

Not built: `eval` and import by name through the GHC API (the `hint`
package), which would find any installed module with no export module, at
the cost of `libHSghc` (about 100 MB), a slow start, and an API that
changes with every GHC.

## Pitfalls

- **An infinite list's length doesn't return**, as in Haskell: `len(xs)`
  or `list(xs)` in Python force the whole list, memory grows, and the RTS
  ends the process when it runs out. Read elements instead: `xs[1000]`,
  `itertools.islice(xs, 10)`, a `for` loop that stops. GHC's `-M` doesn't
  help: an embedded RTS can't deliver heap overflow to a main thread it
  doesn't have.
- **`str()` of a Haskell value isn't its `show`.** Export `'show`, as the
  music-diatonic example does, and call it.
- **A Haskell list passed to Python is a proxy**, not a Python list:
  `json.dumps` refuses it, as it would any foreign object.
- **Nesting depth.** Each call into Haskell from C runs the RTS's scheduler
  on the caller's stack (about 17 KB a level), so a call is refused, as
  `StackOverflow`, once less than 1 MB of stack is left: recursion through
  Haskell stops at about 400 levels, before the core's 1000.
- **Module libraries**: `-dynamic` with no RTS (no `-threaded` or
  `-flink-rts` needed), `-this-unit-id` of their own (two libraries with a
  module of the same name would otherwise share `Typeable` names), and
  `-outputdir`, or ghc leaves objects and `*_stub.h` beside the sources.
- **The binding** needs `-dynamic` (the package is a shared library) and
  `-threaded`; `-rdynamic` to export the program's own modules.
- **Instances with type variables in their heads** (`Show [a]`) aren't
  expanded: `show` has no `[Char]` instantiation (a string goes to `Text`'s).
- **Fork**: the RTS's threads don't survive a C `fork()`. Fork before
  Haskell starts, or not at all.
- **Valgrind**: `--track-origins` never gets through the RTS's 1 TB address
  reservation; run without it.
