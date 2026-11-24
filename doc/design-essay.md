# From Inline::Java to UGLY

*What I learned building language bridges, and how the idea holds up against
GraalVM and the Wasm Component Model in 2026*

## The N² problem

Inline::Java lets Perl code use Java classes as if they were Perl classes. You
`use Inline Java => ...`, and Java objects show up as blessed Perl references.
Method calls, overloaded signatures, arrays, exceptions and even callbacks from
Java back into Perl all work. Getting there took years, and nearly all of that
effort went into problems that have nothing to do with Perl or Java
specifically. Which overload does a Perl scalar select? Who keeps a Java
object alive while Perl holds a reference to it? What happens when Java calls
back into Perl while Perl is waiting on Java?

Every pairwise bridge solves those same problems again: Inline::Python,
JPype, PyO3, J2V8 and all the others. With N languages that means N(N-1)/2
bridges, each with its own answers, its own bugs and its own maintainer.

UGLY was my attempt in 2013 to solve them once. It puts a C core in the middle
and gives each language two adapters: a **plugin**, so the core can drive that
language's runtime, and a **binding**, so code in that language can drive the
core. That makes 2N adapters. A Perl program would call a Python library
through the core, and nobody would ever write a Perl↔Python bridge.

The arithmetic is right. What I underestimated is the condition attached to
it: **2N only holds if the contract in the middle is complete.** Any question
the core leaves unanswered gets answered differently by each plugin, and those
answers have to agree pair by pair. A hub with a thin contract is N² bridges
in disguise.

The rest of this essay covers what the contract has to say, what UGLY's
version said in 2013, and what other systems have since worked out.

## Five things a bridge has to solve

### 1. Types flow from the static side

A Perl scalar holding `"42"` has no type of its own. It might be a string, an
integer or a double. When Inline::Java calls a Java method, it has to choose
among overloaded signatures, so it scores each candidate by how well the Perl
arguments convert. When the scores can't settle it, the user forces the issue
with `Inline::Java::cast("int", $x)`.

The lesson is that **the typed side has to supply type information, and when
it can't, the caller has to.** UGLY builds this into the API:

```c
ugly_value *ugly_object_call_method(ugly_context *ctx, ugly_object *obj,
    ugly_type rtype, const char *method,
    ugly_value **args, int nb_args, const char *hint);
```

`rtype` is the type the caller wants back, so a Perl return value is coerced
to what the caller asked for rather than to whatever Perl happens to have. The
`hint` argument was reserved as the neutral form of `cast`: a free-form string
that a plugin for a statically typed language can use to choose an overload.
No plugin ever used it, but the slot is in the right place.

### 2. Objects are handles, and lifetime belongs to the bridge

Objects can't be converted, only referenced. In Inline::Java, a Java object
that Perl sees stays in a table on the Java side and Perl holds a proxy. When
the proxy's `DESTROY` runs, the bridge removes the table entry and the Java GC
is free to collect the object. This scheme works well until a cycle crosses
the boundary. If a Perl object holds a Java object that holds a callback to
the Perl object, neither garbage collector can see the whole cycle, and it
leaks.

UGLY has the handle (`ugly_object`, wrapping a `void *` that only the plugin
understands) but no ownership model. Freeing is explicit, through
`ugly_object_delete`, and nothing says what happens when a handle gets passed
to a third runtime, or when two runtimes both hold one. In a pairwise bridge
you can settle this locally. In a hub it has to be a rule that every plugin
follows identically, or handles will be freed while still in use in one
direction and leak in the other.

### 3. Callbacks change everything

Callbacks were the hardest part of Inline::Java. By default, the JVM ran in a
separate process and talked to Perl over a socket. When Java needed to call a
Perl sub in the middle of a call, Perl was blocked waiting for the reply. So
Perl's wait loop had to accept and service incoming callback requests on the
same connection, recursively and as deeply as the call chain went.

Running in-process makes the plumbing easier, because a callback is just a
nested C call. It does not make the semantics easier. Each re-entry into a
runtime has to restore that runtime's state: Perl's interpreter context,
CPython's thread state and GIL, a JVM thread attachment. Errors also have to
unwind correctly through frames that belong to other languages.

UGLY has no callback path. That matters more than any other gap, because
callbacks decide whether the core has to be reentrant, and that is not
something you can add later.

### 4. Exceptions must survive the trip

In Inline::Java, a Java exception reaches Perl as a `die` carrying the
exception object, and you can test it with `caught("java.io.IOException")`.
This works because the bridge can map one specific exception model onto one
other specific model.

UGLY stores errors as data in an `ugly_context`: an error code plus a message,
cleared at the start of every call. Keeping C free of exception machinery was
the right decision, and the distinction between `UGLY_NOT_SUPPORTED` (the
language can't do this) and `UGLY_NOT_IMPLEMENTED` (the plugin doesn't do it
yet) is one I'd keep. But a code and a string discard almost everything a
caller needs. A neutral error needs at least these fields:

- the originating language;
- a type name in that language's own terms;
- a message;
- a handle to the original exception object, so that when the error comes
  back to its own runtime it can be rethrown with its identity intact.

### 5. The process boundary was a feature

Inline::Java offered three modes. The JVM ran out of process by default.
`JNI => 1` loaded it into the Perl process, and `SHARED_JVM` let several Perl
processes share a single JVM. The out-of-process default was not laziness. A
separate JVM can't crash Perl, doesn't fight Perl over signal handlers or
threads, and can be restarted on its own.

UGLY deliberately chose in-process, and that is still its niche: no
serialization and no IPC, with real object identity on both sides. But in
exchange it takes on every interpreter's assumptions about owning the process,
including signals, `atexit`, thread-local state and symbol clashes between
libraries loaded with `RTLD_GLOBAL`. In a hub every plugin pays that cost, and
pays it again in combination with every other plugin.

## What UGLY got right

Looking back at the 2013 code, I think a few decisions still hold up:

- **The plugin/binding split.** Treating "call into X" and "X calls in" as two
  separate adapters is what makes the N languages compose.
- **Helpers in the target language.** Most of each plugin is written in its
  own language (`helper.pm` for Perl, and `helper.java` in the original Java
  plugin), and the C part only marshals
  calls. The Perl helper also `use`s the Perl binding to build its return
  values, so a language's plugin and binding share their value code. That cuts
  the real cost per language well below "2 adapters".
- **Errors as data, not exceptions,** with a context that is cleared on entry
  to every call.
- **Caller-declared return types** (`rtype`), plus the reserved `hint`.
- **A generated plugin interface.** `ugly_api.h` is parsed to produce the
  vtable, so the public API and the plugin contract can't drift apart.

What it lacked is everything in the previous section that can't be done
locally: ownership, callbacks, structured errors and threads. The type system
is also too small, with no null, bytes, arrays or maps. The Perl plugin works
end to end because a single plugin never needs any of these rules to agree
with another plugin.

## The landscape in 2026

When I wrote UGLY, the alternatives were pairwise bridges or RPC. Today two
serious systems take on the general problem, and each one resolves exactly
the questions UGLY left open, by changing the premise.

**GraalVM Polyglot / Truffle** puts every language on one VM and one heap.
Ownership and cross-language cycles stop being problems, because there is a
single garbage collector. Interop is a *protocol* rather than a conversion:
objects answer messages such as "are you null?", "do you have array
elements?", "read member *x*" and "execute with these arguments", so a Python
list can be used from JavaScript without being copied. That idea, handles plus
a small set of questions you can ask any foreign value, is the best answer I
know of to the problem of a fixed type system, and it carries over to a C hub.
The price is that you must use Graal's language implementations. GraalPy,
GraalJS, TruffleRuby and Espresso are serious projects, but they aren't
CPython or Perl, and native extensions are the usual sticking point.

**The Wasm Component Model** (WIT, WASI 0.2+) goes the other way: a
shared-nothing design. Each component has its own memory, and values are
copied across the boundary by a canonical ABI according to a WIT interface. It
answers ownership explicitly with **resources**, which are handles passed as
either `own<T>` (ownership moves to the receiver) or `borrow<T>` (valid only
for the duration of the call). That is exactly the language-neutral rule UGLY
was missing, and I would copy it more or less as is. The price is that
everything has to be compiled to Wasm. Python can become a component
(`componentize-py` bundles a Wasm build of CPython), but the native extension
ecosystem does not come along, and Perl or a real JVM are out of reach.

The other entries in the landscape are narrower:

- **Plugin hosts** (Extism, Wasmtime embeddings) are the Component Model's
  premise packaged as a product: a host and sandboxed guests, not peers.
- **Panama FFM**, final since Java 22, replaces JNA for the Java *binding*,
  including upcalls for callbacks. The Java *plugin* still needs the JNI
  invocation API to create the JVM, but once it is running, the helper side
  can use FFM instead of hand-written JNI.
- **Pairwise bridges** (PyO3, JPype, napi-rs, the Inline:: family) are still
  the best way to connect two specific languages. That remains the trade.
- **Out-of-process RPC** (gRPC, Cap'n Proto) is Inline::Java's default mode
  turned into infrastructure: isolation, at the cost of copying and of losing
  object identity.

Laid out like this, UGLY's niche is clear and still mostly empty: **the real,
native interpreters, sharing one process, calling each other through a
neutral contract.** GraalVM requires its own implementations, Wasm requires
recompiling everything, and RPC means leaving the process. If you need CPython
with its C extensions, a real Perl and a JS engine to call each other directly,
nothing in that list does it.

## What a v2 would have to decide first

If I come back to this, I'd want a small, sharp v2, not a broad one. The
decisions below come before writing any plugin:

1. **Values.** Primitives (null, bool, int64, double, string, bytes) cross by
   value. Everything else crosses as a handle, including lists, maps,
   functions and objects. Handles support a small set of Truffle-style
   queries: `is_callable`, `get_member`, `get_index`, `length`, `call`. Eager
   conversion to native lists and maps can be an opt-in convenience, not the
   model.
2. **Ownership.** The core owns a handle table. Handles are `own` or `borrow`,
   with WIT's semantics: borrows end when the call returns, and owned handles
   are reference-counted and released explicitly by bindings, typically from a
   finalizer. Cross-runtime cycles leak, and the documentation says so.
3. **Callbacks.** A callback is a callable handle, with nothing special about
   it. The core is reentrant from the start, and each plugin is responsible
   for restoring its runtime's state on entry (interpreter context, GIL,
   thread attachment).
4. **Errors.** An error is `{language, type, message, handle}` and propagates
   up the stack across every language involved. Each binding turns it into a
   native exception, and when it gets back to its own runtime the original
   object is rethrown.
5. **Threads.** The first rule is the simplest one that is true: a runtime
   belongs to the thread that created it, and the core rejects calls from any
   other thread. CPython's per-interpreter GIL and free-threaded builds may
   justify relaxing this later.
6. **A conformance suite.** One set of tests runs against every plugin and
   covers conversions, handles, ownership, callbacks, errors and teardown. The
   suite *is* the contract. If a plugin passes it, it composes with every
   other plugin. Without such a suite, the 2N claim is unverifiable.

The order matters:

1. **Lua** comes first because embedding it is trivial, which means every
   problem that comes up is a problem with the contract and not with the
   runtime.
2. **Embedded CPython** is second, because the GIL and thread state put the
   reentrancy and threading rules to a real test.
3. **QuickJS** brings a third object model.
4. **Perl** comes last, ported from the v1 plugin. It is the most idiosyncratic
   of the four, and if the contract survives Perl, it's language-neutral.

Java comes back only if there is a reason, and if it does, it uses FFM.

UGLY's original bet was that the hard part of language interop is shared
across all pairs of languages, so it should be written once. I still think
that's right. What I didn't appreciate in 2013 is that "written once" means
specified once, completely, before the second plugin exists.
