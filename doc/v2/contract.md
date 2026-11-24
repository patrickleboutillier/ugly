# UGLY v2: the contract

*Approved 2026-11-24. This document sits alongside
[`ugly.h`](../../include/ugly.h) (the public API) and [`ugly_plugin.h`](../../include/ugly_plugin.h)
(the plugin contract). The headers live in `include/`. The v1 reference is
tag `v0.02`.*

The [design essay](../design-essay.md) ends with this point: "written once"
means specified once, completely, before the second plugin exists. This
document is that specification. Every rule in it should become a test in the
conformance suite (§9). A rule that can't be tested doesn't belong here.

## Scope

**v2.0 covers:** primitives by value; everything else by handle; the
handle protocol (members, elements, call, invoke, new); ownership with
own and borrow; callbacks; structured errors that keep their identity; the
thread rule; lifecycle; and the conformance suite. It ships with plugins
for Lua, CPython and QuickJS, and Perl 5 is ported last.

**Not in v2.0:**
- a hash protocol for non-string keys (Lua tables with table keys, Python
  dicts with tuple keys, JS `Map`);
- iterators and generators;
- async/await and coroutines across runtimes;
- collecting cycles across runtimes;
- sandboxing;
- Java.

Each of these can be added later without breaking the contract.

## What changes from v1

| | v1 (`v0.02`) | v2 |
|---|---|---|
| Values | heap `ugly_value*`, 6 types | `ugly_val` by value; adds null and bytes; int64; strings carry a length |
| Objects | `ugly_object*` wrapper, freed explicitly | `ugly_handle` (index + generation) in a core table, own/borrow |
| Calling | `call_function(name)`, `object_call_method` | `import`/`globals` give handles; `call`, `invoke`, `new` on handles |
| Callbacks | none | callable handles: `ugly_callable_new`, plus foreign functions are handles |
| Errors | code + string in the context | `{status, language, type, message, payload}`, propagated and rethrown |
| Threads | unspecified | each runtime and each ctx belongs to one thread |
| Plugin interface | generated from `ugly_api.h` | written by hand (`ugly_plugin.h`), versioned ABI |
| Bindings | separate code per language | the language's plugin in attach mode |
| Loading | `dlopen("libugly_<x>.so")` through `LD_LIBRARY_PATH` | full path under the plugin dir |
| Teardown | refused while objects are alive | runtime delete invalidates its handles (§7) |

Kept from v1:
- errors as data, cleared by every call;
- the caller states the kind it wants (`want`, formerly `rtype`);
- the split between `NOT_SUPPORTED` and `NOT_IMPLEMENTED`;
- helpers written in the target language;
- the plugin/binding split, now expressed as create mode and attach mode.

## 1. Values

Primitives cross by value: null, bool, int64, double, string (UTF-8, with
an explicit length so NUL is allowed) and bytes. Everything else crosses as a
handle, including lists and maps.

This is a deliberate rejection of eager conversion. Copying a Python list
into a Lua table is a convenience, not a model: it loses identity and
mutation, and it costs O(n) at every crossing. A binding can still offer
"convert to a native list" on top of the element protocol.

Ownership of string and bytes memory follows the general rule: **arguments
are borrowed, results are owned by the caller.** A callee that keeps a
string copies it. A string or bytes result points to memory the value
owns, which `ugly_val_release` frees. Primitives need no allocation at all.

**`want`:** every operation that returns a value takes the kind the caller
needs.
- `UGLY_ANY` means "whatever is natural in the home language".
- Anything else is converted by the **home plugin, using that language's
  rules**, not by neutral rules in the core.
  - Perl numifies `"42"`, because Perl scalars carry no type of their own.
  - Python returns `UGLY_ERR_TYPE` for `"42"` when asked for an int,
    because that conversion doesn't happen implicitly in Python.
  - Both are faithful to the language. Neutral coercion in the core would be
    wrong for one or the other.
- This is essay point 1 (types flow from the static side), with the caller
  as the static side.

v1's `hint` (a string to choose between overloads) is not in v2.0: none of
the four planned languages has overloading. It returns with Java, as an
optional call-options struct.

## 2. Handles and the interop protocol

A handle is a `uint64_t`: a slot index plus a generation counter in the
core's handle table. It is never a pointer, so a stale handle is detected
(`UGLY_ERR_STALE_HANDLE`) rather than dereferenced. Each handle has a
**home runtime**, and the native value lives there. Each table entry holds
the home runtime, the plugin's native reference (`void *ref`), a reference
count and the identity key (§3).

The protocol is a small set of questions you can ask any foreign value.
This is Truffle's idea, reduced:

| Trait | Operations |
|---|---|
| `MEMBERS` | `get_member`, `set_member`, `member_names`: string keys, so fields, methods and dict/hash/table entries |
| `ELEMENTS` | `length`, `get_index`, `set_index`: integer indexes |
| `CALLABLE` | `call` |
| `INSTANTIABLE` | `new` |

Plus `invoke` (a method call) and `describe` (the traits, plus the type name
in the home language's terms).

`invoke` is not "get member, then call". Receivers differ between languages:
- Python returns bound methods;
- JavaScript needs `this`;
- Perl passes the invocant as the first argument;
- Lua's `obj:m()` is sugar for `obj.m(obj)`.

The home plugin knows which one applies, and the caller shouldn't have to.

Modules are just handles. `ugly_rt_import("calc")` returns a handle with
members, and v1's `call_function("calc::add")` becomes "get member `add`,
then call it". `ugly_rt_globals` gives the global namespace.

## 3. Ownership

This follows the Component Model's resources, adapted to a shared address
space. It is essay point 2.

- **Owned handles** are reference-counted in the core. `ugly_dup` adds a
  reference and `ugly_release` drops one. When the count reaches zero,
  the core calls the home plugin's `release(ref)`, and the native value
  becomes collectable in its home runtime.
- **Arguments are borrowed.** Handle arguments are valid for the duration
  of the call. To keep one, the callee calls `ugly_dup`.
- **Borrows are enforced, not just documented.** The core passes each
  handle argument as a *borrow handle*: a distinct slot that points at
  the same entry and is **revoked when the call returns**.
  - A plugin that forgets to dup a handle it keeps fails deterministically
    with `UGLY_ERR_STALE_HANDLE` on next use. Without revocation, it would
    use memory that has been freed, and only sometimes notice.
  - The cost is one table insertion and one removal per handle argument
    per call.
  - Since a borrow slot differs from the owned slot, comparing raw handle
    values doesn't compare identity. `ugly_same` compares the underlying
    entries.
- **Results are owned by the caller.**
- **Proxies.**
  - A foreign handle appears natively as a proxy: a Lua userdata with a
    metatable, a Python object with `__getattr__`/`__call__`, a JS Proxy,
    a blessed Perl object.
  - The proxy owns one reference and releases it from its finalizer:
    `__gc`, `tp_dealloc`, a JS finalizer, `DESTROY`.
  - The native garbage collector therefore decides when the reference is
    released, and the core decides when the object is freed.
- **Unwrapping.**
  - When a handle comes back to its home runtime, the plugin hands its code
    the original native value (`ugly_resolve`), never a proxy.
  - When a plugin converts one of its own proxies to an `ugly_val`, it
    emits the proxied handle, never a proxy of a proxy.
  - Without these two rules, identity breaks after one round trip.
- **Identity.**
  - A plugin exports a native object together with an identity key: the
    `PyObject*`, or the referent `SV*`, or the Lua table pointer. While a
    live handle has the same key, `ugly_export` returns that handle.
  - So exporting the same object twice gives handles that `ugly_same`
    reports as identical.
- **Releasing from any thread.** Finalizers run whenever, and on whichever
  thread, the runtime's GC chooses. `ugly_release` is therefore
  thread-safe.
  - On the home thread it is immediate.
  - From any other thread it is queued and drained at the next entry into
    the home runtime.
  - Plugins must accept `release` while a call is in progress, because a
    finalizer can run in the middle of one.
- **Cycles across runtimes leak.** A Perl object that holds a Lua proxy,
  whose Lua table holds a proxy of that Perl object, is invisible to both
  collectors. This is documented, not solved. The fix would be a
  cross-heap cycle collector, which is a research project.

## 4. Callbacks and re-entry

This is essay point 3. A callback is nothing special: it is a callable
handle.
- A C host makes one with `ugly_callable_new`.
- A foreign function passed as an argument is already one.
- A Lua function given to Python arrives as a proxy whose `__call__` goes
  through `ugly_call`.

The cost is re-entry. Python can call Lua, which calls the C host, which
calls Python again, to any depth. The rules:

- **The core is reentrant.** The per-thread `ugly_ctx` keeps a stack of
  call frames. A frame records its borrows and its error slot, so nested
  calls don't clobber each other.
- **Every plugin entry point saves and restores its runtime's state.** That
  means:
  - perl: `PERL_SET_CONTEXT`, generalising v1's `UGLY_PERL5_ENTER`;
  - Python: the GIL and thread state, with `PyGILState_Ensure`/`Release`;
  - Lua: the right `lua_State` and a protected call boundary;
  - QuickJS: the `JSContext`, plus job-queue draining policy.
- **Errors must cross frames cleanly.** No plugin may `longjmp` across the
  core: Lua errors must be caught with `lua_pcall` at every boundary. The
  same applies to perl `croak` (`G_EVAL`) and C++ exceptions (catch at the
  boundary).

## 5. Errors

This is essay point 4. An error has five parts:
- a status;
- the originating **language**;
- the **type** in that language's terms (`ZeroDivisionError`, `TypeError`,
  `die` for a string die in Perl);
- a **message**;
- a **payload**: a handle to the original exception object.

How an error propagates through three runtimes (Perl calls Lua, Lua calls
Python, Python raises):

1. A Python `ValueError` escapes the function. The Python plugin catches it
   at the boundary and calls `ugly_raise_native`, which creates an error of
   type `ValueError` whose payload is the exception object, exported as a
   handle.
2. The Lua plugin's proxy `__call__` sees `UGLY_ERR_RAISED`. It takes the
   error out of the ctx (`ugly_take_error`) and raises it in Lua as a
   wrapper userdata.
3. Lua code can catch it with `pcall`, inspect the type and message, and
   drop it (`ugly_error_free`). If it doesn't catch it, the wrapper escapes
   back out of Lua.
4. On the way out, the Lua plugin recognises its own wrapper and restores
   the **original** error (`ugly_restore_error`). It does not make a new Lua
   error about an error.
5. Perl receives `die` with an exception object that carries the language
   (`python`), the type (`ValueError`), the message and the payload.
6. If Perl passes that payload back into Python, it arrives as the original
   `ValueError` object, and `except ValueError as e` gets `e is original`.

**Rethrow identity is a requirement, and it's tested.** When a payload
travels back to its home runtime, the original exception object is thrown
again, not a copy.

## 6. Threads

This is essay point 5. The rule is the simplest one that is true:
- **A runtime belongs to the thread that created it.**
- **A ctx belongs to the thread that created it.**

A thread has at most one ctx. The core checks both on every call and returns `UGLY_ERR_WRONG_THREAD`. The
only exception is `ugly_release`, for the reason given in §3.

This rules out handing a runtime between threads, even with external
locking. Allowing that is a v2.x relaxation, if per-interpreter-GIL CPython
or free-threaded builds justify it. Relaxing a rule later is easy;
tightening one later breaks users.

## 7. Lifecycle: languages and runtimes

**Capabilities.** Plugins declare what they can do, instead of failing at
run time. The conformance suite skips by capability, so every gap is
visible in the plugin's declaration rather than hidden in a test log.
- `UGLY_CAP_MULTI_RUNTIME`: several runtimes per process.
- `UGLY_CAP_RESTARTABLE`: can unload and load again in one process.
- `UGLY_CAP_ATTACH`: can attach to an interpreter it didn't create (needed
  for a binding).
- `UGLY_CAP_EVAL`: supports `ugly_rt_eval`.

**Restart is a declared property.** v1 found that
perl can't run `PERL_SYS_TERM` and then start again in the same process.
CPython's `Py_FinalizeEx` followed by `Py_Initialize` is officially
supported, but unreliable with extension modules.
- So `term` is optional. A plugin whose process-wide state is one-way
  leaves it NULL.
- Its `.so` then stays mapped after `ugly_lang_unload`, which also avoids
  `dlclose` problems.
- Lua and QuickJS keep no global state, so they are restartable.

**Runtime teardown changes from v1.**
- In v1, deleting a runtime is refused (`UGLY_IN_USE`) while it has live
  objects.
- In v2, `ugly_rt_delete` always succeeds, and **every handle whose home is
  that runtime becomes stale.** Proxies for those handles in other
  runtimes then fail with `UGLY_ERR_STALE_HANDLE`.
- The reason: in v2, references to a runtime's objects are held by
  *proxies in other runtimes*, and those are released by those runtimes'
  garbage collectors. If deletion were refused while handles are live,
  whether you could tear a runtime down would depend on another
  language's GC timing.
- The new rule is how closing a connection works. Language unload is
  still refused while its runtimes are alive, since that is the host's own
  bookkeeping.

## 8. Bindings are plugins in attach mode

v1 had a plugin (the core drives Perl) and a binding (Perl drives the core)
as separate code. The Perl helper already `use`d the binding to share
value code. v2 makes this structural:
- A binding is the language's plugin in **attach mode**:
  `ugly_rt_attach(lang, existing_interpreter)`.
- The host program's own interpreter becomes a runtime in the core. Its
  objects are exported as handles with the same proxy and unwrapping code,
  and callbacks into the host are ordinary calls into that runtime.
- What remains specific to the binding is a thin native API, such as a
  Python `ugly` module or a Perl `Ugly` package, that wraps `ugly.h`.

So each language needs one adapter with two modes, not two adapters. The
essay's "2N" becomes about N plus N thin API wrappers.

## 9. The conformance suite

The suite is the contract. Each plugin provides **fixtures**: a module
named `conformance`, with the same specification written in its own
language, in `tests/<lang>/` (the mock's are built in). v1's
`calc`/`oocalc` were the start of this. The module provides:

- `echo(x)` returns x.
- `kind_of(x)` returns `null`, `bool`, `int`, `double`, `string`, `bytes`,
  `proxy` (for any foreign value), or the object's type name.
- `make_counter()` returns an object whose type name is `counter`. It has
  an `inc()` method, a `count` field and `items`, a `list` of the counts so
  far. The finalizer count covers the counter and its list.
- `apply(f, x)` returns `f(x)`, and `call_twice(f, x)` returns `f(f(x))`.
- `raise_error(msg)` raises an exception of type `ConformanceError` with a
  `message` member.
- `catch_and_return(f)` returns the exception object `f()` raised. For a
  foreign error, that is its payload.
- `rethrow(e)` raises `e` again.
- `finalized()` counts the fixture objects freed so far, after forcing a
  collection if the language has a GC.
- `Point` has the type name `class` and is instantiable: `new(Point, x, y)`
  gives a `point` with members `x`, `y` and a method `norm2()`.
- `inits()` (optional) counts how often the plugin's process-wide init has
  run. The restart test uses it if it exists.

Type names are whatever `describe` reports, in the language's own terms.
Lua uses each metatable's `__name`, so the fixtures set them.

The tests are C programs. Meson runs each one against every plugin built
(suite `conformance-<plugin>`), and runs `pairwise.c` once for every
ordered pair of different plugins in one process (suite `pairwise`). The
pairwise run is where "2N composes" actually gets proven. The groups:

| Group | Covers |
|---|---|
| values | every kind round-trips through `echo`; int64 extremes; NaN and ±inf; strings with NUL and non-ASCII; bytes; `want` conversions per the language's own rules |
| protocol | traits from `describe`; members, elements and `length`; `invoke` versus get-then-call; `new` |
| ownership | `release` runs the native finalizer (`finalized()` goes up); `dup` keeps the object alive; a stale handle is detected; **a borrowed handle kept after the call is detected**; `ugly_same` after round trips |
| identity | the same object exported twice gives the same handle; proxies unwrap when they return home, so there's no proxy of a proxy |
| callbacks | C → L → C → L to depth 100; runaway recursion fails cleanly, every level unwinds, and the runtime survives; L1 → L2 → L1 across runtimes; the C host owns callable lifetimes |
| errors | type and message are preserved; **the payload rethrown at home is the identical object**; an error crossing three runtimes |
| threads | calls from another thread give `WRONG_THREAD`; a release from another thread is deferred and drained |
| lifecycle | capabilities are honoured; runtime delete makes outstanding handles stale; unload is refused while runtimes are alive; restart is tested only with `RESTARTABLE` |
| pairwise | values, identity, calls, caught and rethrown errors, and alternating recursion between two languages; deleting one language's runtime while the other holds its values |
| memory | the whole suite under the valgrind setup: no errors and no leaks |

## 10. The four planned languages

How each language maps onto the contract. "Proxy finalizer" is where a
proxy releases its handle reference.

| | Lua 5.4 | CPython 3.12+ | QuickJS | Perl 5 |
|---|---|---|---|---|
| null | `nil` | `None` | `null`, `undefined` | `undef` |
| int / double | integer / float subtypes | `int`, or `TYPE` beyond int64 / `float` | number: INT if integral and `want` allows | IV / NV, using SV flags |
| string vs bytes | strings are bytes: STRING if valid UTF-8, else BYTES | `str` / `bytes` | string / `ArrayBuffer` | UTF-8 flag: on → STRING, off → STRING decoded as Latin-1, or BYTES if wanted |
| members | table string keys; metatable `__index`. Every table has both MEMBERS and ELEMENTS; INSTANTIABLE if `t.new` is a function; type name from `__name` | `getattr`, or dict keys for a dict | properties | hash keys; for methods, `can` |
| elements | sequence `1..#t`, shown 0-based | sequence protocol | arrays | arrays |
| `invoke` receiver | `obj:m()` | bound method | `this` | invocant |
| proxy finalizer | userdata `__gc` | `tp_dealloc` | class finalizer | `DESTROY` |
| re-entry | `lua_pcall` at every boundary; Lua allows about 200 nested C calls, so a call costs one | GIL with `PyGILState_*` | context, job queue | `PERL_SET_CONTEXT`, `G_EVAL` |
| restartable | yes | no | yes | no |
| multiple runtimes | yes | no in v2.0 (subinterpreters later) | yes | yes (MULTIPLICITY) |

## Decisions

The review questions, decided on 2026-11-24 (the recommendation was taken
in every case):

1. **Borrows are enforced:** each handle argument gets a borrow slot,
   revoked when the call returns (§3).
2. **Runtime delete invalidates** that runtime's handles. It is no longer
   refused (§7).
3. **A binding is its language's plugin in attach mode** (§8).
4. **`want` is applied by the home plugin, using the language's own rules**
   (§1).
5. **Integers that don't fit in int64 give `UGLY_ERR_TYPE`.** There is no
   silent fallback to DOUBLE or to a string.
6. **Perl strings without the UTF-8 flag are decoded as Latin-1 into
   STRING.** They become BYTES only when `want` asks for bytes.
7. **Deferred releases are drained only on entry into the home runtime.**
   There is no `ugly_rt_collect`.
8. **No hash protocol in v2.0.**

## Clarifications from the first implementation

The core and the mock plugin (2026-11-24) needed these points pinned down.
All of them are in the headers and covered by the conformance suite.

- **One ctx per thread.** A second `ugly_ctx_new` on the same thread is
  `UGLY_ERR_USAGE`. Plugins get the current one with `ugly_current_ctx()`,
  for native code such as a proxy's `__call` that wasn't handed one.
- **Releasing doesn't touch the error.** `ugly_release`, `ugly_val_release`
  and `ugly_error_payload` leave the ctx's error alone, so cleanup after a
  failure can't erase it. `ugly_release` accepts a NULL ctx, for finalizer
  threads that have none.
- **Plugin lookup** uses `UGLY_PLUGIN_PATH`, a colon-separated search path,
  then the compiled-in plugin directory. Plugins are loaded with
  `RTLD_GLOBAL`, because modules loaded by an embedded interpreter (XS, C
  extensions) need its symbols.
- **Loads are counted.** Loading a language that is already loaded returns
  the same `ugly_lang`. Only the last `ugly_lang_unload` is refused while
  runtimes are alive.
- **Runtime deletion** is refused (`UGLY_ERR_IN_USE`) during a call into
  that runtime. Otherwise it releases every live value first, while the
  interpreter still runs, so finalizers run. It repeats this while
  finalizers create new values, and then destroys the interpreter.
- **Host values have a home.** Every ctx owns a runtime of the built-in "c"
  language, which is the home of `ugly_callable_new` callables and of core
  lists and maps. It is a plugin like any other, so the core has no special
  cases for it.
- **Plugin services added:**
  - `ugly_rt_native()`, the plugin's per-runtime state;
  - `ugly_fail()`, a failure with a chosen status in the plugin's language
    and no payload. A missing member is `UGLY_ERR_NOT_FOUND`, not
    `UGLY_ERR_RAISED`.
- **Re-entry has a limit.** Rule 1 originally said "to any depth", and no
  interpreter allows that. The core fails calls nested deeper than
  `UGLY_MAX_DEPTH` (1000 frames) with `UGLY_ERR_TOO_DEEP`, instead of
  overflowing the C stack. A language with a lower limit reports it as an
  error. Lua's is about 200 nested C calls, found by the Lua plugin. The
  suite requires 100 levels to work, and 100000 to fail cleanly with every
  level unwound.
- **`ugly_raise_handle()`** raises an error whose payload the plugin already
  holds as a handle: a foreign exception rethrown by native code (found by
  the Lua plugin).
- **Fixtures are language-neutral.** The exception type is
  `ConformanceError`, `inits()` is optional, and nothing checks a receiver
  convention or a bytes type that a language doesn't have.
- **The mock plugin** (`plugins/mock/`) is a small language written in C,
  with lenient, Perl-like conversions. It implements every rule of
  `ugly_plugin.h` and every conformance fixture.

## Order of work

1. ~~Implement the core from `ugly.h`: handle table, frames, borrows,
   errors and thread checks. Test it against a **mock plugin** written in
   C.~~ Done 2026-11-24 (`src/`, `plugins/mock/`).
2. ~~Write the conformance suite against the mock plugin.~~ Done
   2026-11-24 (`tests/`): 8 groups and 180 subtests, clean under valgrind
   and helgrind.
3. ~~Write the Lua plugin and make it pass the suite.~~ Done 2026-11-24
   (`plugins/lua/`, fixtures in `tests/lua/`). It passes all 8 groups, and
   the pairwise suite passes for mock→Lua and Lua→mock, all clean under
   valgrind.
4. The CPython plugin. Its pairs join the pairwise suite, so Lua↔Python is
   tested from day one.
5. **Bindings: an `ugly` module in each language, and attach mode, for Lua
   and Python together** (§8). The in-language API wraps loading,
   runtimes and import, for example
   `local json = require("ugly").import("python", "json")`. Attach mode lets
   `require "ugly"` / `import ugly`, run from a plain `lua` or `python3`
   process, turn the running interpreter into a runtime of the core. The
   conformance suite gains real attach tests in place of today's skip.
   Milestone: `python3 script.py` uses a Lua module, and `lua script.lua`
   uses a Python one, with no C host.
6. Then QuickJS, then port Perl.
