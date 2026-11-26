# The Haskell bridge: a plan

Status: being built in `plugins/haskell/` (the skeleton, values and boxes,
export tables and calls, module libraries, foreign values in Haskell, and
hint and want for overloads are done). Once it works,
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
- A runtime is plain Haskell state, and has none yet: modules are
  libraries, loaded once per process (below). GHC has no global
  interpreter state, so runtimes are independent and cheap:
  `UGLY_CAP_MULTI_RUNTIME`.
- The threaded RTS takes calls from any OS thread: `UGLY_CAP_ANY_THREAD`.
  Values are immutable, so nothing needs a lock except a runtime's module
  table (`atomicModifyIORef`). No lock is held while calling out.
- No `UGLY_CAP_EVAL` at first (see "Later").
- The `rts` option passes RTS flags (`-N4`, `-M1g`); the RTS also reads
  `GHCRTS`. Pitfall: an invalid RTS flag makes `hs_init` exit the process,
  as it would for any GHC program.
- The RTS keeps every CAF (`keep_cafs`). Module libraries are dlopen'ed
  after it starts, and nothing tells it that their foreign exports are
  roots: GHC 9 processes those only in `hs_init` and in its own linker. So
  a top-level value, such as a module's box, could otherwise be collected
  while code that enters it is still reachable. The cost: a CAF that grows
  (`[0..]` at top level) is never freed.

Three libraries: `ugly_haskell.so` is the vtable in C, built by Meson;
`libugly_rts.so` is the small C file that starts the RTS, which links it,
built by ghc because only ghc knows where its RTS headers are (they moved
in 9.4); and `libugly_hs.so` is the Haskell side, the package `ugly`
(`-this-unit-id ugly`), with no RTS of its own. Module libraries are built
against that package, so they share its types, and Template Haskell loads
it into ghc while compiling them, where it runs on ghc's own RTS.

### Modules: compiled export tables

A module that other languages can use is a small Haskell file, its export
list written by hand (decision 4):

```haskell
{-# LANGUAGE TemplateHaskell #-}
{-# OPTIONS_GHC -Wno-orphans #-}
module Music.Diatonic.Ugly (uglyExports) where
import Ugly.Export
import Music.Diatonic
import Music.Diatonic.Chord
import Music.Diatonic.Scale

uglyModule "Music.Diatonic"
  [ ''Note, ''Interval, ''Scale, ''Chord
  , 'C, 'D, 'E, 'F, 'G, 'A, 'B, 'raise, 'lower, 'transpose, 'above, 'below
  , 'Min3rd, 'Maj3rd, 'majorScale, 'minorScale, 'majorChord, 'tonic, 'notes ]
```

`uglyModule` (Template Haskell, `Ugly.Export`) is built, and tested by
`haskell-exports` on a test library (`Example`, exported by
`Example.Ugly`). It declares `uglyExports`, the module's box:

- **A type name** (`''Note`) gets `ToUgly` and `FromUgly` instances, unless
  it has them: a box whose text is its `show` (if it has `Show`), and whose
  methods (`invoke`, receiver last) are this module's exports. Only types
  of kind `*`. The instances are orphans, hence `-Wno-orphans`.
- **A value name** (`'transpose`, a constructor `'C`, a class method) is an
  export. A polymorphic one is expanded into its instantiations: for each
  constraint in its context, the instances in scope whose heads match it
  (a two-parameter class with a fundep binds both variables), checking the
  instances' own contexts (`Ord [a]` needs `Ord a`); a variable with no
  constraint becomes `Value`. Instantiations that need a type with no
  `ToUgly`/`FromUgly` are left out silently: `Ord` and `Show` have
  instances for many types that can't cross (`largest :: Ord a => [a] ->
  a` keeps those that can). An export with none left, or with a type
  variable of a higher kind (`Functor f`), is a compile error.
- Instances are those visible where the table is compiled, which is after
  the library, so all of its instances are included. An instance defined
  later in another package isn't. That's Haskell's open world, made
  explicit.
- `uglyModule` also declares the module's root: a foreign export,
  `ugly_hs_module_` plus the name encoded (ASCII letters and digits as they
  are, any other UTF-8 byte as `_` and two hex digits:
  `ugly_hs_module_Music_2eDiatonic`), that gives a stable pointer to its
  box. `uglyModuleFrom "Smoke" 'smoke` declares one for a box written by
  hand with `Ugly.Value` (`moduleBox`).

### Module libraries

Built. An export module is compiled, with the library it exports and
anything else it needs, into a **module library**: a `.so` built against
the package `ugly`, with no RTS, which joins the plugin's.

```
ghc -dynamic -shared -fPIC -package-db <build>/plugins/haskell/package.db -package ugly \
    -this-unit-id music-diatonic-ugly -o Music.Diatonic.so Music/Diatonic/Ugly.hs
```

- **The package `ugly`** is registered in a package database in the build
  tree (`plugins/haskell/package.db`, by `package.py`). Its interfaces are
  `.dyn_hi`, which `ghc -dynamic` looks for in a package. Meson builds
  module libraries with `ghc_module` (the command) and
  `ghc_module_depends`; the plugin's own tests build theirs in
  `plugins/haskell/test/`. Not installed yet: a database for an installed
  plugin, with the interfaces beside it, comes with the first module
  library built outside the tree (music-diatonic).
- **`import "A.B.C"`** looks for the module's root symbol in the process
  first (a host program that links its export modules), then in
  `<dir>/A.B.C.so`, `<dir>/A.B.so`, `<dir>/A.so`, the longest prefix first,
  for each directory of the `path` option, then of `UGLY_HASKELL_PATH`. So
  one library can hold several modules (`Example.so` has `Example` and
  `Example.Points`). A library that can't be loaded stops the search:
  `UGLY_ERR_NOT_FOUND`, `ModuleNotFound`, with the loader's message. A
  name with a `/` is never a path.
- **Libraries are never closed**, as the RTS holds pointers into their
  code, and they're opened `RTLD_LOCAL`, so two of them can each hold a
  Haskell module of the same name. Modules are found once per process and
  cached: every runtime gets the same box, and the same identity.
- **Pitfall: give each library its own unit** (`-this-unit-id`). Two
  libraries built as the default unit `main` that both have a module
  `Utils` link apart, but their types have the same `Typeable`
  fingerprint, so a value of one would pass for the other's.

### Values

Built (`Ugly.Value`, `Ugly.Plugin`), and tested by `haskell-values`
against the test module `Smoke` (`plugins/haskell/test/`). A `ToUgly` instance says how each
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
| a function, or an `IO` action | `CALLABLE` |
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
- **Calls are curried.** Fewer arguments than the arity give a partial
  application, itself a callable (`add(2)` is an `Int -> Int`). More are
  refused (`UGLY_ERR_TYPE`, too many arguments), and so is a call with
  none. A pure function's result is evaluated to weak head normal form; an
  `IO` result is run, and an `IO` action is called with no arguments.
- **`invoke`**: on a module, its function, with no receiver; on another
  box, a function of the module that exported its type, with the receiver
  last (decision 1): `pt.scale(0.5)` is `scale 0.5 pt`.
- **`describe`** gives the type's name as `Typeable` shows it: `Color`,
  `[Int]`, `Maybe (Maybe Int)`, `Map [Char] Int` (a `String` is `[Char]`
  to `Typeable`).
- **Leaks**: the refs live in the RTS's stable pointer table, which
  valgrind can't see, so the Haskell side counts them (`ugly_hs_live_refs`)
  and the test checks that none is left once the runtime is deleted.

### Polymorphism: overloads chosen at the call

A polymorphic export is a set of monomorphic instantiations, like a Java
method's overloads (`overloaded`), and a call picks one the same way, by
cost (built):

1. **Argument types.** Each instantiation matches the arguments by its
   parameter types alone, without evaluating anything, and each argument's
   conversion has a cost (below). A Haskell value coming home carries its
   exact type, so `tonic(scale)` picks `Scale -> Note` at cost 0; a foreign
   primitive matches every parameter type it converts to. The cheapest
   instantiation is called; a tie is refused as ambiguous (`UGLY_ERR_TYPE`,
   listing them), as is a call no instantiation takes.
2. **The `hint` call option** names the type when arguments don't decide
   (built): `transpose(Min3rd, above, x, opts(hint="Chord"))`. A hint is
   type arguments, as `TypeApplications` writes them: `"Chord"` (or
   `"@Chord"`) fixes the first type variable, `"@Scale @Note"` the first
   two, and `"@_ @Note"` leaves the first open. Variables are in the order
   of the type's `forall` (a class method's class variable first). Types
   compare as `Typeable` shows them, ignoring spaces and outer parentheses,
   with `String` as `[Char]`. Only the instantiations the hint names are
   considered; arguments that don't fit them are `UGLY_ERR_TYPE`, and a
   hint that names none is `UGLY_ERR_NOT_FOUND`, listing the hints that
   would (as Java's hint naming no overload). A hint on a function with no
   type variables is `UGLY_ERR_NOT_FOUND` too. Every polymorphic export is
   overloaded, even with one instantiation, so a hint always applies.
3. **`want` ranks instantiations by their result** (built), for functions
   polymorphic in their result alone (`read`, `minBound`): `read("42")`
   wanting an int is `read @Int`, wanting a double `read @Double`, and
   `read("C#")` with a hint of `Note` gives a `Note`. Each result type has
   a cost as each kind (`ToUgly`'s `wantCost`), mirroring the argument
   costs: an int is `Int` (0), `Int64` or `Integer` (1), a narrower type
   (2), or a widened integer as a double (3); a box is a handle, or a
   string (2) if it has a text. This is the essay's "types flow from the
   static side", where the caller is the static side, and it's the one
   place in UGLY where `want` chooses the function as well as the
   conversion. With want `ANY`, `read("42")` is ambiguous, and the message
   lists every instantiation with the hint that picks it.

A result that can't be the kind wanted is refused before anything runs,
monomorphic or not (`Pt(1, 2)` wanting an int is `UGLY_ERR_TYPE`, its
result can't be one). A polymorphic value (`minBound`) is overloaded too,
called with no arguments: `minBound()` wanting a bool is `False`.

Keyword arguments and a call context are refused (`UGLY_ERR_NOT_SUPPORTED`):
Haskell has neither.

**Limit**: instances whose heads have type variables of their own (`Show
[a]`, `Read (Maybe a)`) aren't expanded, so `label` has no `String`
instantiation (its `String` argument goes to the `Text` one). Expanding
them one level, over the types in scope, is the natural next step if a
real library needs it.

A type variable with no constraint becomes `Value`, which takes anything:
`id` works on any value, and `[Value]` takes any sequence that comes home,
read through its elements, so `head` works on any list.

### Arguments: a foreign value enters as a literal would

Haskell has no implicit conversions. It has overloaded literals:
`42 :: Double` is `fromInteger 42`, and with `OverloadedStrings`, `"x" ::
Text` is `fromString "x"`. A foreign primitive arriving where a Haskell
type is expected converts the way a literal of that kind would (built,
`FromUgly`), at a cost that ranks overloads:

| Argument | Cost 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| int | `Int` | `Int64`, `Integer` | a narrower type it fits (`Word8`, `Int32`, `Natural`...) | `Double`, `Float` | `Value` |
| double | `Double` | | `Float` | | `Value` |
| string | `String` | `Text` | `Char`, if one character | | `Value` |
| bytes, bool | `ByteString`, `Bool` | | | | `Value` |
| null | `()`, `Nothing` | | | | `Value` |
| a Haskell value | its own type | | | | `Value`; `[Value]` for any sequence |

- An int that doesn't fit is refused (decision 2), so `widen(300)` with a
  `Word8` parameter is `UGLY_ERR_TYPE`, and an overload on `Int8` or
  `Word8` is chosen by the value's range.
- Never through `Read`: a string isn't a `Note`, even though `read "C#"`
  is one.
- A double isn't an `Int`: no truncation.

So Haskell is strict in the way Python is, not lenient like Perl.

### Foreign values in Haskell

Built (`Ugly.Types`, `Ugly.Marshal`, and the module `Ugly` for Haskell
code), and tested by `haskell-foreign` with values whose home is the C host
(callables, a core list and map).

- **A foreign value is a `Foreign`**, or a `Value` holding one (`VForeign`):
  an owned handle (the borrowed argument, duplicated when the call takes
  it), released when Haskell drops it. The finalizer is a Haskell one
  (`Foreign.Concurrent`), run on the RTS's finalizer thread, never inside
  the GC, since a release can run plugin code; off the value's home thread,
  the core defers the release to it.
- **An export takes one** as a `Foreign` parameter (cost 0), a `Value`
  (cost 4), or `[Value]` for a sequence: its length when the call takes it,
  each element when it's forced. Going out again it's its own handle
  (rule 5: `identity(x)` is `x`).
- **Operations on it are in `IO`** (module `Ugly`): `call`, `invoke`,
  `get`, `index`, `size`, `members`, `typeOf`, with `toValue` and
  `fromValue` (the argument rules, a refusal thrown as a `TypeError`). They
  go through the core on the calling thread's ctx, so a runtime bound to
  another thread refuses them (`WRONG_THREAD`), as it should. A result
  whose home is the current Haskell runtime is its box; the current
  runtime is per OS thread, set by each entry point (`glue.c`), since each
  call from C runs as a bound thread on its caller's OS thread.
- **A foreign callable passed where a Haskell function type is expected**
  (`transpose`'s `Interval -> Note -> Note`) becomes a Haskell function
  (`Collect`): it gathers its arguments, then makes one call with all of
  them, so `f 3 4` is `f(3, 4)`, not `f(3)(4)`. Its result converts by
  `FromUgly`. It's pure, through `unsafePerformIO`, unless its result is
  in `IO`: that's the FFI's own precedent, as a `foreign import` of a C
  function may be declared pure. A pure one is called when its result is
  forced, and its errors are thrown from there, as `error` would be. A
  lazy result keeps it: `map f [1, 2, 3]` returned as a list calls `f`
  when an element is read, after the call that passed it has returned.
- In an export table, a function-typed parameter needs the other way
  round: its parameters `ToUgly`, its result `FromUgly` (`usableType`).
- Rule 5: a Haskell value coming home, such as `above` passed back to
  `transpose`, resolves to the original closure, with no conversion.
- Every call out uses a `safe` foreign import, which releases the RTS
  capability. So a callback into Haskell works, from the same thread or
  another one.
- A Haskell value passed out from Haskell code (a box among a call's
  arguments) is exported to the current runtime for the call. Code with no
  current runtime (a thread Haskell forked) can't pass one.

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
  `throwIO`, the same value (rule 7): an `ErrorCall` that went out through
  a Python callback is caught in Haskell as that `ErrorCall` (built).
- **Foreign errors in** (built): any failed operation, an exception or not
  (`NOT_FOUND`, `WRONG_THREAD`), is a `ForeignError` exception that owns the
  taken `ugly_error`, with its status, language, type, message and payload
  copied for Haskell. Haskell can `catch` it; if it escapes, the plugin
  restores the original error, unchanged. Thrown again after that (Haskell
  kept it), it's raised from its copies. A caught one is freed by a weak
  reference's finalizer.
- **A callback's result that doesn't convert** is a `TypeError` thrown
  where it's forced: `UGLY_ERR_RAISED` if it escapes, not `UGLY_ERR_TYPE`,
  as it happened inside Haskell code.
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
The program's own export modules are found in the process (linked with
`-rdynamic`, so their root symbols are visible), with no library. The
program's RTS wasn't started with `keep_cafs`, so attaching must call
`setKeepCAFs()` before any module library loads.

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
3. **Export tables and calls** (done): `uglyModule`, with instantiation;
   arguments in (`FromUgly`, the literal rules); calls, currying, `IO`;
   overloads; `invoke` with the receiver last.
4. **Module libraries** (done): building one against the `ugly` package (a
   package database in the build tree), and `import` from the module path,
   replacing the built-in modules. Then foreign values in Haskell (done:
   foreign values, callbacks, foreign errors), and `hint` and `want` for
   overloads (done).
5. **The fixture** (`tests/haskell/Conformance.hs`), then the protocol until
   the suite and pairwise pass.
6. **music-diatonic** as an example, from Python and Lua.
7. **The binding**, then the docs.

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
