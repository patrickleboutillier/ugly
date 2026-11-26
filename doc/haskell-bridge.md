# The Haskell bridge: a plan

Status: proposal, being built in `plugins/haskell/` (the skeleton, then
values and boxes, are done). Once it works,
this becomes the language guide (`doc/languages/haskell.md`), Haskell's
column in contract §10, and a section of the plugin guide. Until then it's
the place to argue about the design.

Haskell is the language that tests the contract's assumptions hardest, as
TODO.md said. Every other plugin embeds an interpreter that can find a
module by name, list an object's members and call a function with whatever
arguments arrive. GHC compiles: at run time there's no interpreter, no
reflection, and no function that takes "any arguments". Values are
immutable, evaluation is lazy, functions are curried, and most interesting
functions are polymorphic through type classes. The plan below keeps
the contract as it is and moves the missing reflection to compile time.

## What the spikes established

All with GHC 8.8.4 (Ubuntu 22.04); CI will add 9.4 (24.04).

- **The RTS starts in a dlopen'ed library.** A C host dlopens a `.so` built
  with `ghc -dynamic -shared -threaded -no-hs-main`, which calls
  `hs_init_ghc` with `--install-signal-handlers=no`. No signal handler
  changes: the skeleton's smoke test checks all of them. The threaded RTS's
  timer is a thread (timerfd), not a signal.
- **Any thread can call in.** Four pthreads calling foreign exports at once
  work; each call runs as a bound Haskell thread on the calling OS thread.
- **A second Haskell library joins the running RTS.** A module `.so` built
  separately, with no RTS linked, dlopened after startup, runs against the
  RTS already there. That is how modules will load.
- **Exceptions stop at the boundary.** `try` around `evaluate` catches a
  `div` by zero inside the foreign export. An exception that escapes a
  foreign export ends the process, so every entry point will catch
  everything.
- **References and identity.** A `StablePtr` keeps a value alive with an
  address C can hold. A `StableName`, taken after forcing the value to weak
  head normal form, gives the same key for the same heap object every time.
- **Valgrind can run it, without `--track-origins`.** The only leaks are
  "possibly lost" blocks from the RTS's threads, which are still alive
  because the RTS is never stopped (suppressed in `tests/valgrind.supp`).
  But the RTS reserves 1 TB of address space at startup, and with
  `--track-origins=yes` valgrind never gets through it: even a plain
  `main = print 1` doesn't finish. GHC 9.x can shrink the reservation
  (`-xr`); 8.8 can't. So suite `ghc` (anything that starts the RTS) has its
  own setup, `valgrindghc`, the same checks without origins, and is left
  out of `valgrind`. Helgrind reports dozens of races in the RTS's
  scheduler, which synchronizes in ways it doesn't model, so `ghc` is out
  of helgrind too; ThreadSanitizer on the core is the check for threads.
- **Template Haskell can expand polymorphism.** For each name, `reify` gives
  its type, and `reifyInstances` finds every instance of the classes in its
  context. On music-diatonic:

  ```
  transpose :: Nte a => Interval -> (Interval -> Note -> Note) -> a -> a
    becomes 5 functions, for a = Note, Degree, Scale, Key, Chord
  tonic :: Deg s n => s -> n            (a two-parameter class with a fundep)
    becomes Scale -> Note, Chord -> Note, Harmony -> Chord
  notes :: Nts a => a -> [Note]
    becomes Scale, Signature and Chord versions
  id :: a -> a
    becomes Value -> Value       (no constraint: the universal type)
  ```

  The spike is in `stuff/haskell/spike/` (local, not in the tree).

## The shape

### The runtime

- `init` starts the RTS once per process, and nothing stops it (`hs_exit`
  then `hs_init` isn't supported). No `term`, no `UGLY_CAP_RESTARTABLE`.
- A runtime is plain Haskell state: the modules it has loaded. GHC has no
  global interpreter state, so runtimes are independent and cheap:
  `UGLY_CAP_MULTI_RUNTIME`.
- The threaded RTS takes calls from any OS thread: `UGLY_CAP_ANY_THREAD`.
  Values are immutable, so nothing needs a lock except a runtime's module
  table (`atomicModifyIORef`). No lock is held while calling out.
- No `UGLY_CAP_EVAL` at first (see "Later").
- The `rts` option passes RTS flags (`-N4`, `-M1g`); the RTS also reads
  `GHCRTS`. Pitfall: an invalid RTS flag makes `hs_init` exit the process,
  as it would for any GHC program.

Two libraries: `ugly_haskell.so` is the vtable in C, built by Meson, and
`libugly_hs.so` is the Haskell side, built by ghc, which also compiles the
small C file that starts the RTS. Only ghc knows where its RTS headers are,
and they moved in 9.4. The Haskell modules are a package of their own
(`-this-unit-id ugly`), so that module libraries built later link against
the same symbols.

### Modules: compiled export tables

A module that other languages can use is a small Haskell file compiled into
a library:

```haskell
module Music.Diatonic.Ugly where
import Ugly.Export
import Music.Diatonic
import Music.Diatonic.Chord
import Music.Diatonic.Scale

uglyModule "Music.Diatonic"
  [ 'C, 'D, 'E, 'F, 'G, 'A, 'B, 'Note, 'Sharp, 'Flat
  , 'raise, 'lower, 'transpose, 'above, 'below, 'Min3rd, 'Maj3rd
  , 'majorScale, 'minorScale, 'majorChord, 'tonic, 'dominant, 'notes ]
```

- `uglyModule` (Template Haskell) generates the export table: one entry per
  name, each with its instantiations (above), the argument conversions and
  the result's operations, all decided at compile time, where the types
  are known.
- Built with `ghc -dynamic -shared` against the `ugly` package, into
  `Music.Diatonic.so`. `import("Music.Diatonic")` looks for it on the
  module path (a `path` option, and `UGLY_HASKELL_PATH`), dlopens it and
  reads its table.
- The names are listed by hand, because TH can't list a module's exports.
  Later, a build helper could read them from the `.hi` file
  (`ghc --show-iface`).
- Instances are those visible where the table is compiled, which is after
  the library, so all of its instances are included. An instance defined
  later in another package isn't. That's Haskell's open world, made
  explicit.

### Values

Built (`Ugly.Value`, `Ugly.Plugin`), and tested by `haskell-values`
against the built-in module `Ugly.Smoke`. A `ToUgly` instance says how each
type crosses. Primitives cross by value:

| Haskell | ugly |
|---|---|
| `()` | null |
| `Bool` | bool |
| `Int`, `Int8`..`Int64`, `Word8`..`Word32` | int |
| `Word`, `Word64`, `Integer`, `Natural` | int if it fits; otherwise a box whose text is its digits (never wrapped) |
| `Double`, `Float` | double |
| `String`, `Text`, `Char` | string; a `String` holding a lone surrogate (valid `Char`, not Unicode) is a box with no text |
| `ByteString` | bytes |
| `Maybe a` | null for `Nothing`, `x` for `Just x`; a box, one element long, when `x` is itself null (`Just Nothing`, `Just ()`) |

Everything else is a **box** (`Box`): the value, and closures over it for
what the protocol can ask: its type name (from `Typeable`), its text, its
members, its elements. They are made where the value's type is static (an
instance, an export table entry), so no class dictionary or reflection is
needed at run time. A ref is a `StablePtr` to the box. Its identity is the
index of the value's `StableName`, plus one, taken after forcing the value
to weak head normal form and kept alive with the ref, so it stays unique:
the same value read twice is the same handle.

| Haskell value | Traits |
|---|---|
| a list, a tuple | `ELEMENTS` (read only) |
| a `Map String v` | `MEMBERS` (read only) |
| a module | `MEMBERS`: its exports, in order |
| a type with `Show` (`boxShow`), or none (`boxOpaque`) | none: a type name, and a text if it has `Show` |
| a function, or an `IO` action | `CALLABLE` (not yet) |
| a record | `MEMBERS`: its fields (not yet) |

- **`want`, by Haskell's rules: strict.** An int widens to a double (the
  suite requires it); nothing else converts implicitly. A number asked for
  as a string is `UGLY_ERR_TYPE`, as in Python: Haskell's `show` is never
  implicit. A box asked for as a string gives its text, since a box has no
  other primitive form: `"Green"`, `"[2,3,5,7,11]"`, `"(1,\"one\")"`. A
  list's or tuple's text is built from its elements' (`textOf`), so it
  needs no `Show` instance of its own.
- **Nothing is mutable.** `set_member`, `set_index`, `splice` and
  `delete_member` are refused with `UGLY_ERR_NOT_CAPABLE`. An `IORef` or
  `MVar` could be a one-member box later.
- **Lists are lazy.** `length` forces the spine (and doesn't return for an
  infinite list); `get_index` walks to the element and forces only it, so
  `[0..]` has a 1000th element, and `[1, error "x", 3]` raises at index 1
  only. Index i costs i steps: iterating is quadratic. Memoizing an array
  per box would fix that but force the whole spine; revisit with a real
  use.
- **Failures** that aren't exceptions use Haskell's names where it has
  them: `TypeError` (Control.Exception's, for deferred type errors) for a
  refused conversion or trait, `ArrayException` for an index out of range,
  `NotInScope` for a missing member, `ModuleNotFound` for a missing module.
- **Exceptions** raise where a value is forced, inside the entry point's
  catch: type is the exception's type name (`ErrorCall`,
  `ArithException`), message is `displayException`, payload a box of the
  `SomeException`.
- **Calls are curried** (not yet). Fewer arguments than the arity give a
  partial application, itself a callable. More are applied to the result,
  if it's a function. A pure function's result is evaluated; an `IO`
  result is run.
- **`describe`** gives the type's name as `Typeable` shows it: `Color`,
  `[Int]`, `Maybe (Maybe Int)`, `Map [Char] Int` (a `String` is `[Char]`
  to `Typeable`).
- **Leaks**: the refs live in the RTS's stable pointer table, which
  valgrind can't see, so the Haskell side counts them (`ugly_hs_live_refs`)
  and the test checks that none is left once the runtime is deleted.

### Polymorphism: overloads chosen at the call

A polymorphic export is a set of monomorphic instantiations, like a Java
method's overloads, and a call picks one the same way, by scoring:

1. **Argument types.** A Haskell value coming home carries its exact
   `TypeRep`, so `tonic(scale)` picks `Scale -> Note`. A foreign primitive
   matches every parameter type it converts to.
2. **The `hint` call option** names the type when arguments don't decide:
   `transpose(Min3rd, above, x, opts(hint="Chord"))`.
3. **`want` picks the result type** for functions polymorphic in their
   result alone (`read`, `fromInteger`, `minBound`): `read("C#")` with
   `want` = int is refused, and with a hint of `Note` gives a `Note`.
   This is the essay's "types flow from the static side", where the
   caller's `want` is the static side, and it's the one place in UGLY where
   `want` chooses the function as well as the conversion.

A type variable with no constraint becomes `Value`, the universal type,
which holds any box or foreign value. So `id`, `map` and `foldr` work on
anything.

### Arguments: a foreign value enters as a literal would

Haskell has no implicit conversions. It has overloaded literals:
`42 :: Double` is `fromInteger 42`, and with `OverloadedStrings`, `"x" ::
Text` is `fromString "x"`. A foreign primitive arriving where a Haskell
type is expected converts the way a literal of that kind would:

- an int to any `Num` type, range-checked (`UGLY_ERR_TYPE` outside it,
  where `fromInteger` would wrap silently);
- a double to `Double` or `Float`;
- a string to `String`, `Text`, or another `IsString` type;
- never through `Read`: a string isn't a `Note`, even though `read "C#"`
  is one.

So a Haskell value is strict in the way Python is, not lenient like Perl.

### Foreign values in Haskell

- A foreign value is a `Value` (`Ugly.Value`): a handle, released by a
  finalizer, which may run on any thread; the core queues that release.
- Operations on it are in `IO`: `get`, `call`, `invoke`, `index`.
- **A foreign callable passed where a Haskell function type is expected**
  (`transpose`'s `Interval -> Note -> Note`) becomes a pure Haskell
  function, through `unsafePerformIO`. That's the FFI's own precedent: a
  `foreign import` of a C function may be declared pure. Its errors are
  thrown from pure code, as `error` would be.
- Rule 5: a Haskell value coming home, such as `above` passed back to
  `transpose`, resolves to the original closure, with no conversion.
- Every call out uses a `safe` foreign import, which releases the RTS
  capability. So a callback into Haskell works, from the same thread or
  another one.

### Laziness and errors

- **Force at the boundary.** Every entry point evaluates what it returns to
  weak head normal form inside its catch, and converting to a primitive
  forces the whole value (a `String`). Otherwise an `error` in a thunk
  would escape after the catch and end the process.
- A lazy structure fails where it's forced: `get_index` on `[1, undefined]`
  at 1 is `UGLY_ERR_RAISED` with an `ErrorCall`. `length` of an infinite
  list doesn't return, as in Haskell.
- **Haskell exceptions out**: type is the exception's type name
  (`ErrorCall`, `ArithException`), message is `displayException`, and the
  payload is the boxed `SomeException`. Coming home, it's rethrown with
  `throwIO`, the same value (rule 7).
- **Foreign errors in**: a `ForeignError` exception that owns the taken
  `ugly_error`. Haskell can `catch` it; if it escapes, the plugin
  recognises it and restores the original error.
- `StackOverflow` and `HeapOverflow` are ordinary asynchronous exceptions,
  and are caught like the rest. Running out of memory with no `-M` limit
  goes through the RTS's `outOfHeapHook`, which exits; that's a pitfall to
  document, not to fix.

### Threads and fork

- Each call from C runs as a bound thread, on the caller's OS thread. So
  Haskell calling Lua from a call that came from that Lua is on the right
  thread. Haskell code that `forkIO`s and calls a thread-bound runtime from
  there gets `UGLY_ERR_WRONG_THREAD`, which is correct.
- **Fork**: the RTS's worker threads don't survive a C `fork()`, like the
  JVM's and the CLR's: forking once Haskell has started is the host's to
  avoid. GHC's own `forkProcess` shows how the RTS could be told; revisit
  if a host needs it.

### The binding: a Haskell program as host

The natural Haskell user of UGLY is a Haskell program that wants Python's
or Java's libraries. The binding is the `ugly` package itself, linked into
that program:

```haskell
import qualified Ugly

main = Ugly.withUgly $ do
  json <- Ugly.module_ "python" "json"
  s <- Ugly.invoke json "dumps" [Ugly.toValue [1, 2, 3 :: Int]]
  putStrLn =<< Ugly.fromValue s
```

It registers the plugin (`ugly_lang_register`) and attaches to the
program's own RTS (`UGLY_CAP_ATTACH`). Haskell values passed to Python are
then boxes in that runtime, so Python can call back into the program.

## Later

- **`eval` and import by name, through the GHC API** (the `hint` package).
  It would find any installed module with no export file, and evaluate
  expressions, by running the same instantiation as `uglyModule` at run
  time. The cost: `libHSghc` (about 100 MB), a slow start, and an API that
  changes with each GHC version. Worth it only once the compiled path
  works.
- An `IORef`/`MVar` as a mutable box; `Data.Map` with non-string keys,
  through the hash protocol in TODO.md.

## The order of work

1. **The skeleton** (done): the build, the RTS's start, runtimes, a smoke
   test (`haskell-smoke`, suites `haskell` and `ghc`), CI.
2. **Values and boxes** (done): `Ugly.Value`, the primitive conversions,
   boxes and their identity, `release`, `describe`, and the text for `want`
   = string, from a built-in module (`Ugly.Smoke`) until modules load.
3. **Export tables and calls**: `uglyModule`, with instantiation; arguments
   in (`FromUgly`, the literal rules); calls and currying; building a module
   library against the `ugly` package (a package database in the build
   tree); `import` from the module path, replacing `Ugly.Smoke`.
4. **The fixture** (`tests/haskell/Conformance.hs`), then the protocol until
   the suite and pairwise pass.
5. **music-diatonic** as an example, from Python and Lua.
6. **The binding**, then the docs.

## Decided (2026-11-26)

The author's answers to the questions of idiom and feel:

1. **`invoke` on a box: the receiver goes last.** Haskell puts the subject
   last, so that `x & f a b` works. So `chord.transpose(Min3rd, above)`
   means `transpose Min3rd above chord`, among the exports of the module
   that exported `Chord`. On a module, `invoke` calls its function with no
   receiver.
2. **Integers that don't fit are refused** (`UGLY_ERR_TYPE`), never wrapped
   as `fromInteger` would. This also lets overload resolution rule out an
   instantiation whose integer type is too small, as Java's scoring does.
   Going out, an `Integer` beyond int64 is a box of its digits.
3. **`Maybe` is flattened**: `Nothing` is null, `Just x` is `x`, except that
   `Just x` stays a box (one element long) when `x` is itself null, so
   nothing is lost.
4. **Export lists are written by hand** (`uglyModule` with names). A tool
   that drafts one from the GHC API can come later.
