# UGLY v2: the contract

*Approved 2026-11-24. This document sits alongside
[`ugly.h`](../include/ugly.h) (the public API) and [`ugly_plugin.h`](../include/ugly_plugin.h)
(the plugin contract). The headers live in `include/`. The v1 reference is
tag `v0.02`.*

The [design essay](design-essay.md) ends with this point: "written once"
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
| `MEMBERS` | `get_member`, `set_member`, `member_names`, `delete_member`: string keys, so fields, methods and dict/hash/table entries |
| `ELEMENTS` | `length`, `get_index`, `set_index`, `splice`: integer indexes |
| `CALLABLE` | `call` |
| `INSTANTIABLE` | `new` |

Plus `invoke` (a method call) and `describe` (the traits, plus the type name
in the home language's terms).

`delete_member` and `splice` came with libugly 2.1 and plugin ABI 2
(2026-11-25), for bindings that make foreign containers native (Perl's
`delete`, `push`, `pop`, `shift`, `splice`):
- `delete_member` removes a member; one that isn't there is not an error.
  Values whose members are fixed (a Java object, a mock object) refuse it
  with `UGLY_ERR_NOT_CAPABLE`.
- `splice(offset, remove, insert...)` replaces a range, resizing the
  sequence. One operation covers append, insert, remove, truncate and
  clear. The core checks the range against `length` first
  (`UGLY_ERR_NOT_FOUND` outside it), so plugins see only valid ones. A
  sequence of fixed size (a Java array, a Python tuple) refuses it with
  `UGLY_ERR_NOT_CAPABLE`.
- A plugin that leaves either slot NULL gets `UGLY_ERR_NOT_IMPLEMENTED`
  from the core, as for any operation.

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

**Relaxed for UGLY_CAP_ANY_THREAD** (libugly 2.2, 2026-11-25). Hosts like
HAProxy run their Lua from several worker threads, one Lua state per thread,
and those threads all need the one Python of the process.
- A plugin with `UGLY_CAP_ANY_THREAD` promises that every entry point, its
  release and runtime deletion included, may run on any thread,
  concurrently. It serializes what needs it itself, and holds no lock of
  its own while calling out through the core, since another thread may be
  calling back in. Python (the GIL, released around its calls out) and
  Java (a `JNIEnv` per thread, attached as a daemon) have it.
- Entry and deletion can't race: entering counts itself in, then looks for
  a deletion; deletion marks the runtime closing, then looks for entries.
  One of the two sees the other and backs off (`UGLY_ERR_STALE_HANDLE`, or
  `UGLY_ERR_IN_USE`). The deleting thread may still enter, for finalizers.
- Releases to such a runtime are always queued and drained by the next
  entry, from any thread: a release done on the spot could race with a
  deletion on another thread.
- `ugly_rt_get` shares such a runtime instead of making another: the ctx
  holds a share; one `ugly_rt_get` created is deleted with its last share,
  on whichever thread drops it; one deleted while shared is dead at once
  and freed with its last share.
- `ugly_ctx_thread()` gives a thread its ctx, creating one that is deleted
  when the thread exits. The Python and Java bindings use it, so threads
  their host language starts work too: they share Python and Java, and get
  a Lua of their own.
- Other runtimes stay with their thread. Lua, Perl and QuickJS can't be
  entered by a second thread while one is inside, and a lock around them
  would deadlock against callbacks; a host that serializes them itself
  (HAProxy's shared `lua-load` state) is phase 2, in TODO.md.

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

**Options** (libugly 2.1, plugin ABI 3, 2026-11-25). Bindings create
runtimes on demand (`ugly_rt_get`), so the host has no `rt_new` call at
which to pass settings. `ugly_configure(ctx, lang, key, value)` records them
beforehand:
- Options are lists of strings, per language name, and process-wide. Each
  call appends; a NULL value clears the key.
- They must be set before the language starts. Afterwards the call fails
  with `UGLY_ERR_IN_USE`, because a JVM's heap or Python's `sys.path` at
  init can't change later. "Starts" means loads for the first time: a
  plugin that stays initialized after unloading (no `term`) can't be
  configured again, but one torn down by `term` can.
- Each plugin declares the keys it takes (`options` in `ugly_plugin`), and a
  load with any other key fails with `UGLY_ERR_USAGE`, so a typo can't go
  unnoticed.
- Plugins read them with `ugly_option(lang, key, i)`, ahead of the
  environment variables that play the same part:

| Language | Keys | Ahead of |
|---|---|---|
| Lua | `path`, `cpath` (`package.path`, `package.cpath`) | `LUA_PATH`, `LUA_CPATH` |
| Python | `path` (`sys.path`) | `PYTHONPATH` |
| JS | `path` (module directories) | `UGLY_JS_PATH` |
| Perl | `path` (`@INC`) | `PERL5LIB` |
| Java | `classpath`, `options` (JVM options: `-Xmx512m`, `-Dk=v`...) | `UGLY_JAVA_CLASSPATH`, `CLASSPATH` |

Every binding has `configure`: `ugly.configure("java", options=["-Xmx512m"])`
in Python, `ugly.configure("python", { path = "./py" })` in Lua and JS,
`Ugly::configure(python => { path => [...] })` in Perl, and
`Ugly.configure("python", "path", dir)` in Java.

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

**As built (Lua, Python, JS and Perl, 2026-11-24; Java, 2026-11-25).**

- **One source file, compiled twice.** Each plugin's source is built as the
  plugin (`ugly_<lang>.so`, linked with the interpreter library) and as the
  binding: the native module `ugly` (a Lua C module, a Python extension),
  linked with no interpreter library.
  - The binding's interpreter symbols come from the host process.
  - This is required, not just tidy: Debian's `python3` and `lua5.4` are
    statically linked. Loading the plugin's `.so` into them would put a
    second interpreter in the process.
- **The binding registers instead of loading.** On import, it calls
  `ugly_lang_register()` with the plugin compiled into it, then
  `ugly_rt_attach()` on the host's interpreter. After that,
  `ugly_lang_load("lua")` (or `"python"`) returns this language, so
  everything else finds the host's interpreter.
- **The same `ugly` module everywhere.** Every runtime has the module
  preloaded: `require "ugly"` in Lua, `import ugly` in Python. Embedded
  code (in a runtime a C host created) and attached code (a plain `lua` or
  `python3` program) use the same API:
  - `ugly.module(lang, name)` gives a module of any language. It's not called
    `import` because that's a keyword in Python.
  - `ugly.eval(lang, code)` gives the value of code in any language.
  - In Lua, `ugly.is_error(e)` and `ugly.is_proxy(x)`; in Python,
    `ugly.Error`, `ugly.Proxy` and `ugly.rethrow(e)`.
- **Which runtime.** `ugly.module` and `ugly.eval` use
  `ugly_rt_get(ctx, lang)`: the oldest runtime of that language on this
  thread, whoever created it, or else a new one that the ctx owns and
  deletes. So Lua embedded in a Python program reaches back into that
  program's own interpreter.
- **Teardown.** The binding deletes what it created when the host shuts
  down: a `__gc` sentinel in Lua (`os.exit(code, true)` or the end of the
  script closes the state), and `atexit` in Python.
- **JS** (`qjs`): `qjs` loads a native ES module (`import "…/ugly.so"`) by
  calling its `js_init_module`, and `qjs` is linked `-rdynamic`, so the
  binding uses its engine.
  - Teardown has to happen before `qjs`'s `JS_FreeRuntime`, which asserts
    that no object is still referenced, and other languages may hold JS
    values at exit. QuickJS has no runtime-finalizer hook.
  - So a sentinel object on the global object declares every reference the
    binding holds to QuickJS's GC (`gc_mark`): the boxed values behind
    handles, and the cached objects. At exit, the whole graph becomes
    garbage together, and the sentinel's finalizer tears down, releasing
    exactly what it declared.
  - The binding must come from the same QuickJS release as the `qjs` it
    runs in. The tests build both from the same pinned wrap.
- **Perl** (`perl`): `use Ugly` loads `Ugly.pm`, which `XSLoader`s
  `auto/Ugly/Ugly.so`. That file is `perl5.c` again, linked without
  libperl, since `perl` is statically linked and exports its API.
  `boot_Ugly` registers the plugin and attaches to the running interpreter.
  Teardown runs from `Ugly.pm`'s `END` block, before global destruction.
- **Java** (`java`, 2026-11-25): `ugly.Foreign`'s static initializer loads
  `libugly_jni.so` (`System.loadLibrary("ugly_jni")`, or the path in
  `-Dugly.library`). That file is `java.c` again, linked without libjvm.
  Its `JNI_OnLoad` registers the plugin and attaches on the loading thread,
  with that thread's `JNIEnv` as the existing interpreter. When UGLY created
  the JVM, it registered the natives itself and sets `-Dugly.embedded=true`,
  so nothing is loaded.
  - The API is `ugly.Ugly`: `module(lang, name)`, `eval(lang, code)` and
    `isForeign(x)`. Foreign values are `ugly.Foreign` (`call`, `invoke`,
    `get`, `set`, `length`...), and errors are `ugly.ForeignException`.
  - Teardown is at JVMTI's **VMDeath** event. The java launcher calls
    `DestroyJavaVM` on the thread that ran `main`, and `System.exit` from
    `main` stays on that thread too, so the ctx is still there and JNI still
    works. Shutdown hooks can't do it, because they run on new threads,
    which own no ctx. If the JVM ends from another thread, UGLY is left as
    a process exit leaves it.
  - `main`'s `JNIEnv` dies at `DetachCurrentThread`, before
    `DestroyJavaVM` reattaches the thread, so teardown uses the one VMDeath
    passes.
  - Only the thread that loaded the binding can use it. Other Java threads
    get `IllegalStateException` (TODO.md: any-thread runtimes).
- **Perl's sugar** (2026-11-25). Proxies are native through overloads and
  ties: `$p->(...)`, `$p->m(...)`, `$p->{k}`, `$p->[i]`, and the whole tie
  interfaces: `exists`, `delete`, `keys`, `push`, `pop`, `shift`, `unshift`,
  `splice`, `$#a = n` and list assignment (through `delete_member` and
  `splice`). `use Ugly python => 'json'` binds a module to a Perl package:
  - its callable members are subs, defined on first use, so `json::dumps(...)`
    and `json->dumps(...)` both work;
  - its other members read as subs (`math::pi()`);
  - its classes become nested packages with `new`
    (`conformance::Point->new(3, 4)`, `java::util::ArrayList->new`);
  - `use Ugly lua => { string => 'LuaString' }` names the package.

  The Perl side is `plugins/perl5/helper.pm`, embedded in the plugin at
  build time.

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
| protocol | traits from `describe`; members, elements and `length`; `invoke` versus get-then-call; `new`; `splice` (replace, append, remove, clear, the range check) and `delete_member` (exact, or refused where members are fixed) |
| ownership | `release` runs the native finalizer (`finalized()` goes up); `dup` keeps the object alive; a stale handle is detected; **a borrowed handle kept after the call is detected**; `ugly_same` after round trips |
| identity | the same object exported twice gives the same handle; proxies unwrap when they return home, so there's no proxy of a proxy |
| callbacks | C → L → C → L to depth 100; runaway recursion fails cleanly, every level unwinds, and the runtime survives; L1 → L2 → L1 across runtimes; the C host owns callable lifetimes |
| errors | type and message are preserved; **the payload rethrown at home is the identical object**; an error crossing three runtimes |
| threads | calls from another thread give `WRONG_THREAD`, or work with `UGLY_CAP_ANY_THREAD`; a release from another thread is deferred and drained; for `UGLY_CAP_ANY_THREAD`, 4 threads at once in the shared runtime (`ugly_rt_get`), and a runtime deleted while another thread holds a share |
| lifecycle | capabilities are honoured; runtime delete makes outstanding handles stale; unload is refused while runtimes are alive; restart is tested only with `RESTARTABLE` |
| pairwise | values, identity, calls, caught and rethrown errors, and alternating recursion between two languages; deleting one language's runtime while the other holds its values |
| memory | the whole suite under the valgrind setup: no errors and no leaks |

## 10. The languages

How each language maps onto the contract: the four planned ones, and Java,
added after the plan was complete. "Proxy finalizer" is where a proxy
releases its handle reference.

| | Lua 5.4 or 5.3 | CPython 3.10+ | QuickJS (`js`) | Perl 5 | Java (JDK 21) |
|---|---|---|---|---|---|
| null | `nil` | `None` | `null`, `undefined` (NULL goes in as `null`) | `undef` | `null` |
| int / double | integer / float subtypes | `int` (`TYPE` beyond int64) / `float`; no implicit str↔number or float→int | number: INT if integral and within ±2^53, else DOUBLE; INT beyond 2^53 goes in as `BigInt`, and a `BigInt` comes out as INT if it fits | IOK → INT, NOK → DOUBLE, POK → STRING; booleans by `SvIsBOOL` (5.36+) or `PL_sv_yes`/`PL_sv_no`'s signature; an explicit `want` numifies only what `looks_like_number` | boxed integers and `BigInteger` (if it fits) → INT, `Double`/`Float` → DOUBLE; INT goes in as `Long`, and narrows to `int`/`short`/`byte` parameters when it fits; integers widen to DOUBLE |
| string vs bytes | strings are bytes: STRING if valid UTF-8, else BYTES | `str` / `bytes` | string (STRING if valid UTF-8, no lone surrogates) / `ArrayBuffer` | UTF-8 flag on → STRING; off → STRING if valid UTF-8, else BYTES; `want = STRING` decodes unflagged strings as Latin-1 (see decision 6) | `String`/`Character` / `byte[]`; strings are converted from UTF-16 by the plugin, not with JNI's modified UTF-8 |
| members | table string keys; metatable `__index`. Every table has both MEMBERS and ELEMENTS; INSTANTIABLE if `t.new` is a function; type name from `__name` | `getattr`, or dict keys for a dict | properties, own or inherited; type name is the constructor's name, `module` for a namespace | a hash's keys, then an object's methods; a package's subs and nested packages. Modules and classes are packages (`Ugly::Package`); `new` instantiates | public fields and methods, then bean getters (`getX`, `isX`); a `Map`'s keys. Classes are imported by name; a class with a public constructor is INSTANTIABLE (`class`), else `module` |
| elements | sequence `1..#t`, shown 0-based | sequence protocol | arrays | arrays | arrays and `List`s |
| `invoke` receiver | `obj:m()` for a method found through the metatable; `obj.f()` for a plain field (a module's function) | bound method | `this` | invocant for an object; none for a package's function | the object for an instance method; none for a class's static method. Overloads are chosen by scoring how well the arguments convert |
| proxy finalizer | userdata `__gc` (a foreign function is a C closure over that userdata, so Lua's C library takes it) | `tp_dealloc` of `ugly.Proxy` | class finalizer of the proxy classes (exotic properties; `call` for functions and constructors) | `DESTROY` | a `Cleaner` on `ugly.Foreign`; a foreign value passed where an interface is expected becomes a dynamic proxy (the single abstract method calls it) |
| foreign method call | `obj:m(x)` and `obj.m(x)` both invoke `m` when the member is callable | `obj.m(x)` invokes `m` when the member is callable | | | `f.invoke("m", x)` on `ugly.Foreign`, or through the interface it was passed as |
| re-entry | `lua_pcall` at every boundary; Lua allows about 200 nested C calls, so a call costs one | GIL with `PyGILState_*` | one context per runtime; pending jobs run when the outermost call returns; QuickJS's own stack limit (about 340 levels here) | `PERL_SET_CONTEXT` on every entry; `G_EVAL`; calls run in list context and collapse (none → null, one → it, more → array) | a JNI local frame per operation; exceptions are checked after every JNI call; deep recursion ends in `StackOverflowError` (about 200 alternating levels here) |
| restartable | yes | no | yes | no | no (`DestroyJavaVM` is one-way) |
| multiple runtimes | yes | no in v2.0 (subinterpreters later); initialized once per process, never finalized | yes | yes (MULTIPLICITY); `PERL_SYS_INIT3` once per process, never `PERL_SYS_TERM` | no: one JVM per process, created at plugin load, or the one already running |

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
6. ~~**Perl strings without the UTF-8 flag are decoded as Latin-1 into
   STRING.** They become BYTES only when `want` asks for bytes.~~
   **Revised when the Perl plugin was written (2026-11-24):** an unflagged
   string is STRING if it is valid UTF-8 (which includes all ASCII), and
   otherwise BYTES. Latin-1 decoding applies when the caller asks for
   `want = STRING`. Two reasons:
   - Decoding as Latin-1 turned every BYTES value that passed through Perl
     into a STRING, so bytes couldn't round-trip.
   - Perl programs commonly carry *undecoded UTF-8* (text read from files),
     which Latin-1 decoding would garble.

   It's the same rule as Lua's, whose strings are also bytes.
7. **Deferred releases are drained only on entry into the home runtime.**
   There is no `ugly_rt_collect`.
8. **No hash protocol in v2.0.**

Perl's open questions, decided on 2026-11-24 when the Perl plugin was
written (the recommendations were taken):

9. **Context.** Calls into Perl run in list context and collapse: no value
   gives null, one gives that value, several give an array. `return @list`
   works, and so does a sub returning one value. Perl code can ask for
   scalar context itself with `scalar(...)`. A call option for context can
   come with keyword arguments (TODO.md).
10. **Numbers and dualvars.** With `want = ANY`, the flags decide: IOK gives
    INT, NOK gives DOUBLE, POK gives STRING. An explicit `want` follows
    Perl's own rule, but numifies only what `looks_like_number`; otherwise
    it's `UGLY_ERR_TYPE`.
11. **Booleans.** `SvIsBOOL` on perls that have it (5.36+). On older perls,
    the signature of `PL_sv_yes`/`PL_sv_no` and their copies: all three
    flags, with `"1"`/`""`.
12. **`DESTROY`** runs when the last reference goes, as Perl's refcounting
    does. Releasing a handle runs it immediately. Cycles leak, as Perl's own
    do, and `perl_destruct` runs the rest when the runtime is deleted.

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
- **Conversions that depend on the language are checked as "exact or
  refused"** (found by the Python plugin). Perl and Lua numify `"42"`;
  Python refuses. The suite accepts either, but never a wrong value or
  another status.
- **Single-runtime plugins are checked, not skipped silently.** A plugin
  without `UGLY_CAP_MULTI_RUNTIME` must refuse a second runtime with
  `UGLY_ERR_NOT_SUPPORTED`, and the tests that need two runtimes report a
  SKIP. Cross-runtime scenarios for such a language are covered by the
  pairwise tests.
- **Receivers across languages** (found pairing Lua with Python). A proxy
  turns a callable member into a method call: `obj.m(x)` from Python, and
  `obj:m(x)` or `obj.m(x)` from Lua, all `ugly_invoke(obj, "m", x)`. The
  home plugin then applies its own receiver rule. Lua passes `self` only to
  methods found through the metatable, so a module's plain functions work
  from Python too.
- **Perl** (2026-11-24) needed no change to the contract beyond its
  decisions (9–12, and the revision of 6).
  - It was written from the v1 lessons: `PERL_SET_CONTEXT` on every entry,
    `PERL_SYS_INIT3` once, and never `PERL_SYS_TERM`.
  - Foreign values are `Ugly::Proxy` objects: `&{}`, `@{}` and `%{}`
    overloads, `AUTOLOAD` for methods, `==` for identity, and `DESTROY` to
    release.
  - Modules and classes are `Ugly::Package` objects; their identity is the
    stash.
  - Writing the binding found a bug: `pl_eval` pushed a mark before
    `eval_sv`, which pushes its own. That corrupted the calling Perl
    code's stack frame. The conformance suite couldn't see it, because
    there the host is C; it showed up only with Perl code running
    underneath.
  - The binding test sets `PERL_DESTRUCT_LEVEL=2`, so the host `perl` frees
    everything at exit, and valgrind measures only UGLY. That leaves zero
    leaks.
- **QuickJS** (2026-11-24) needed no change to the contract.
  - It's built from a pinned, checksummed release (`subprojects/quickjs.wrap`), because it isn't commonly packaged.
  - Its `FinalizationRegistry` callbacks are queued only when the GC runs, so its `ugly` module has `gc()` (collect, then run them), and the `finalized()` fixture uses it.
  - `JS_FreeRuntime` asserts that nothing is still referenced, which makes every test's runtime deletion a leak check for the plugin.
  - The valgrind run found a use-after-free in its rule-7 path: it released the error before taking its own reference to the payload's value. That warning is now part of rule 7 in `ugly_plugin.h`.
- **Java** (2026-11-25) needed no change to the protocol, and added one
  rule about signals.
  - One JVM per process: the invocation API can't create a second, or a new
    one after `DestroyJavaVM`. So the plugin is neither MULTI_RUNTIME nor
    RESTARTABLE; it is ATTACH, for the binding (§8).
  - Most of the protocol is Java (`ugly.Bridge`, reflection); C converts
    values, maps exceptions and implements `ugly.Foreign`'s natives.
    Identity is an id from an `IdentityHashMap`, because objects move.
  - Overloads are resolved by scoring each candidate on how well the
    arguments convert, the best total winning. A foreign value passed for an
    interface becomes a dynamic proxy, so a Lua function works as a
    `Function<Object, Object>`.
  - Errors from other languages are `ugly.ForeignException` (`type()`,
    `language()`, `payload()`); one that escapes Java goes on unchanged, and
    `new ForeignException(payload)` rethrows a foreign value (rule 7).
  - **Signals belong to whoever installed them first.** The JVM installs
    SIGSEGV, SIGFPE, SIGILL and SIGBUS handlers it depends on (implicit null
    and divide checks), and `perl_construct` sets SIGFPE to `SIG_IGN`. So a
    plugin must keep another runtime's handlers when its interpreter
    starts: the Perl plugin restores SIGFPE. Found by `pairwise-java-perl5`
    under `-Xcheck:jni`, which checks the handlers. Rule in `ugly_plugin.h`.
  - **The JVM writes to stderr**, through its `vfprintf` hook, so its
    diagnostics can't corrupt a host's stdout (or TAP). `UGLY_JAVA_CHECK`
    turns on `-Xcheck:jni` and makes a JNI warning fatal once the operation
    that caused it returns. The tests set it.
  - The JVM is excluded from the valgrind setup (suite `jvm`): its JIT and
    GC are beyond what memcheck can judge.
- **Lua 5.3** (2026-11-25), for HAProxy, whose builds commonly use it.
  One Lua per process, since every version exports the same `lua_*`
  symbols: the plugin's Lua is a build option (`-Dlua_version`), and the
  language is `"lua"` either way. The binding is built for every Lua found,
  because each loads only into its own host. The code difference was one
  call (`lua_newuserdatauv`); the whole suite and the 5.3 binding pass
  unchanged.
- **Foreign functions are Lua functions** (2026-11-25, found writing
  README examples). Lua's C library checks for real functions:
  `string.gsub(s, p, pyfunc)` refused callable userdata. A foreign value
  that is CALLABLE but not INSTANTIABLE is now pushed as a C closure over
  its proxy, cached per handle like the proxy, and unwrapped by its
  upvalue (rule 5). Classes stay userdata, and so does a class reached as
  a member, which used to become a method closure and hide its members
  (`datetime.date.today()`).
- **Rule 7, completed** (found by the bindings). When an error returns to
  the language its payload came from, that language raises the original
  value or exception object, not a wrapper. A Python exception that crosses
  Lua is caught in Python as itself (`except Mine as e: e is mine`). A Lua
  error value that crosses Python comes back `rawequal` to the original.
- **New core calls for bindings:** `ugly_lang_register()` (plugin header),
  and `ugly_rt_get()` and `ugly_rt_find()` (public API).
- **Lua state** (found by the bindings). The Lua plugin keeps its runtime in
  the registry, not in `LUA_EXTRASPACE`, which belongs to the host when it
  is attached. It also converts values on the `lua_State` it was called
  with, which matters when a proxy is called from inside a coroutine.
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
4. ~~The CPython plugin.~~ Done 2026-11-24 (`plugins/python/`, fixtures in
   `tests/python/`). It passes all 8 groups, and the pairwise suite passes
   for every pair of mock, Lua and Python, in both directions, clean under
   valgrind and helgrind.
5. ~~Bindings for Lua and Python, with attach mode~~ (§8, "As built"). Done
   2026-11-24. A plain `python3` program uses Lua modules, and a plain
   `lua5.4` program uses Python modules, with no C host
   (`tests/python/test_binding.py`, `tests/lua/test_binding.lua`, suite
   `bindings`). Attach mode is tested there, since a C test can't host an
   interpreter it doesn't link.
6. ~~QuickJS~~ (`plugins/js/`, fixtures in `tests/js/`). Done 2026-11-24.
   It passes all 8 groups, and all 12 ordered pairs of mock, Lua, Python and
   JS pass, clean under valgrind and helgrind.
7. ~~Port Perl~~ (`plugins/perl5/`, fixtures in `tests/perl5/`, binding in
   `plugins/perl5/binding/`). Done 2026-11-24. It passes all 8 groups. All
   20 ordered pairs of mock, Lua, Python, JS and Perl pass, and plain
   `perl` programs use the other languages. Everything is clean under
   valgrind and helgrind.

The plan is complete. What comes next is in [TODO.md](../TODO.md). Java
came first (2026-11-25, `plugins/java/`, fixtures in `tests/java/`): it
passes all 8 groups and all 30 ordered pairs of six plugins pass. Its
binding followed the same day: plain `java` programs use the other
languages.

v1 (the 2013 contract, its Perl plugin and XS binding) was removed from the
tree on 2026-11-24, once v2 had two real languages calling each other. It is
kept in tag `v0.02`, and the Perl port starts from there.
