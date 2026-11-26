# JavaScript (QuickJS)

JavaScript on [QuickJS](https://bellard.org/quickjs/), built from a pinned,
checksummed release (`subprojects/quickjs.wrap`), language `"js"`. How JS
maps onto the protocol is the QuickJS column of [the contract's §10](../contract.md#10-the-languages);
this page is the practical side.

QuickJS is a small, complete ES2023 engine, not Node.js: there's no npm,
`fs`, `http` or CommonJS. Node is in [TODO.md](../../TODO.md).

## From JS: the binding

The binding is the native module `plugins/js/binding/ugly.so`, which the
`qjs` built beside it imports (`qjs` loads native modules by path):

```js
const ugly = await import("/path/to/ugly.so");

const functools = ugly.module("python", "functools");     // a module of another language
functools.reduce((a, b) => a * b, [1, 2, 3, 4, 5]);       // 120: Python calls a JS function
const date = ugly.module("python", "datetime").date(2026, 11, 25);
date.isoformat(); date.year;                              // "2026-11-25", 2026
```

| Function | What it does |
|---|---|
| `ugly.module(lang, name)` | imports a module of `lang` in this thread's runtime of it |
| `ugly.eval(lang, code)` | evaluates code in a language that can (`UGLY_CAP_EVAL`) |
| `ugly.configure(lang, { key: value or [ ... ] })` | options for a language that hasn't started ([Configuring](#configuring)) |
| `ugly.kw({ name: value })` | keyword arguments, passed last |
| `ugly.opts({ hint: ..., context: ... })` | other call options, passed last (beside a `kw`, in either order) |
| `ugly.isProxy(v)`, `ugly.isError(e)` | whether a value is foreign, or an error from another language |
| `ugly.gc()` | runs QuickJS's collector, then its finalization callbacks |

Embedded runtimes (JS called from another language) have the same API as
the built-in module `"ugly"`: `import * as ugly from "ugly"`.

## Foreign values in JS

A foreign value is a proxy object, one per value (`===` is identity).

- `p.x`, `p.x = v`: members. `p[0]`, `p[i] = v`, `p.length`: elements,
  0-based (`length` only for values with elements).
- `p.m(x)` invokes `m` with the foreign language's receiver rules. A member
  that is a class stays the class.
- A foreign function or class is a function proxy: `f(x)` calls it, and
  `new Cls(x)` constructs. Calling a class that can only be instantiated
  (a Java or Perl class) constructs it too.
- Members of a foreign value can't be deleted from JS: `delete p.k` does
  nothing (TODO).

## JS values elsewhere

| JS | goes out as | comes back as |
|---|---|---|
| `null`, `undefined` | NULL | `null` |
| boolean | BOOL | boolean |
| number | INT if integral and within ±2^53, else DOUBLE | number (INT beyond 2^53: `BigInt`) |
| `BigInt` | INT if it fits in int64 | number or `BigInt` |
| string | STRING (no lone surrogates) | string |
| `ArrayBuffer` | BYTES | `ArrayBuffer` |
| any other object, function | a handle | the original value |

- Members are properties, own or inherited. Elements are arrays.
- The type name is the constructor's name (`Object`, `Array`, a class's
  name), and `module` for a module namespace.
- `invoke` calls with `this` bound to the object.

## JS as a guest

`ugly_rt_import` loads an ES module: `<name>.js` from the module path
(`UGLY_JS_PATH`, or the `path` option); `"ugly"` is built in.
`ugly_rt_globals` is `globalThis`, and `eval` evaluates a script.

## Errors

An error from another language is thrown as an object inheriting `Error`,
with `type`, `message`, `language` and `payload`. If it escapes JS
unhandled, it goes on unchanged. A JS exception going out has its `name`
as the type (an `Error`'s), and the thrown value is the payload; it comes
back to JS as the same object.

```js
try { json.loads("{oops"); }
catch (e) { if (ugly.isError(e)) print(e.language, e.type); }   // python JSONDecodeError
```

## Threads

A QuickJS runtime belongs to the thread that created it: calls from another
thread fail with `UGLY_ERR_WRONG_THREAD`.

## Configuring

Key: `path` (module directories), ahead of `UGLY_JS_PATH`:
`ugly.configure("js", path=["./js"])` from Python.

## Pitfalls

- **Not Node.** No npm packages, no `require`, no I/O modules beyond
  QuickJS's own `std` and `os` (in `qjs`).
- **Numbers.** An integral double such as `2.0` goes out as INT, because JS
  can't tell it from `2`; integers beyond 2^53 come in as `BigInt`, so
  `typeof` differs and mixing them with numbers throws.
- **Promises and finalizers run late.** Pending jobs run when the outermost
  call into the runtime returns, not during it, and when JS is the host
  (`qjs`), the host's event loop runs them. Finalizers on proxies run only
  when QuickJS collects: call `ugly.gc()` to release foreign values
  promptly.
- **Stack depth.** QuickJS has its own stack limit (about 340 nested levels
  of alternating calls here); deep cross-language recursion fails there
  with QuickJS's "stack overflow" error, cleanly.
- **`JS_FreeRuntime` checks for leaks.** QuickJS asserts that nothing is
  still referenced when a runtime is freed; that's UGLY's bug to fix if you
  see it, not yours.
