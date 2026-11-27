# Lua

Lua 5.4 or 5.3, chosen at build time (`-Dlua_version=auto|5.4|5.3`). The
language is `"lua"` either way. How Lua maps onto the protocol is the Lua
column of [the contract's §10](../contract.md#10-the-languages); this page is
the practical side.

## From Lua: the binding

The binding is the C module `ugly`, built for every Lua found
(`plugins/lua/binding/<version>/ugly.so`, installed into Lua's own C-module
directory). Put its directory on `LUA_CPATH` if it isn't installed:

```sh
LUA_CPATH='build/plugins/lua/binding/5.4/?.so;;' lua5.4 script.lua
```

```lua
local ugly = require "ugly"

local json = ugly.module("python", "json")          -- a module of another language
local data = json.loads('{"scores": [3, 1, 4]}')
print(data.scores[1], #data.scores)                  --> 3   3   (1-based, as in Lua)

local t = ugly.eval("python", "{'a': 1}")            -- an expression's value
print(ugly.is_proxy(t), t.a)                         --> true   1
```

| Function | What it does |
|---|---|
| `ugly.module(lang, name)` | imports a module of `lang` in this thread's runtime of it |
| `ugly.eval(lang, code)` | evaluates code in a language that can (`UGLY_CAP_EVAL`) |
| `ugly.configure(lang, { key = value or { ... } })` | options for a language that hasn't started ([Configuring](#configuring)) |
| `ugly.kw{ name = value }` | keyword arguments, passed last |
| `ugly.opts{ hint = ..., context = ... }` | other call options, passed last (beside a `kw`, in either order) |
| `ugly.iter(v)` | a function for the generic `for` over a foreign value's own iteration: `for x in ugly.iter(p) do` |
| `ugly.is_proxy(v)`, `ugly.is_error(e)` | whether a value is foreign, or an error from another language |
| `ugly.shared_by_threads()` | declares a state several host threads take turns in (HAProxy's `lua-load`) |

The binding attaches on the first use of `ugly`, not at `require`, on the
thread that uses it, and tears down when the state closes. Exit with
`os.exit(code, true)` so the state does close.

## Foreign values in Lua

A foreign value is a proxy: `ugly.proxy` userdata, one per value (`==` is
identity).

- `p.x`, `p.x = v`: members. `p[1]`, `p[i] = v`, `#p`: elements, **1-based**.
  Assigning one past the end appends.
- `p:m(x)` and `p.m(x)` both invoke `m` with the foreign language's receiver
  rules (Python's bound method, Java's `this`...). A member that is a class
  stays the class, so `mod.Point(1, 2)` constructs and `mod.Point.origin`
  still reads a member.
- A foreign function that isn't a class is a real Lua function (a C
  closure), so Lua's library takes it: `string.gsub(s, "%w+", py.str.capitalize)`.
- Calling a class that can only be instantiated (a Java or Perl class)
  constructs it.
- **Iteration** (contract §2): `for x in ugly.iter(p) do` runs what the
  foreign language's loop runs (a Python generator, a list, a dict's
  keys), one value per step, so the foreign code runs only as far as the
  loop goes, and `break` is fine on an infinite one. `ugly.iter` returns a
  Lua function as it is. A foreign value its language doesn't iterate
  raises an error, even if it has elements: index it with `#p` and `p[i]`.
- `tostring(p)` is the foreign language's stringification (Python's
  `str()`, Java's `toString()`), or `ugly.proxy(...)` for a value it has
  none for (a core list).

```lua
local itertools = ugly.module("python", "itertools")
for x in ugly.iter(itertools.count(1)) do      -- an infinite Python iterator
  if x > 3 then break end
  print(x)                                       --> 1  2  3
end
```

## Lua values elsewhere

| Lua | goes out as | comes back as |
|---|---|---|
| `nil` | NULL | `nil` |
| boolean | BOOL | boolean |
| integer / float | INT / DOUBLE | integer / float |
| string | STRING if valid UTF-8, else BYTES | string (both) |
| table, function, userdata | a handle | the original value |

- A table has both MEMBERS and ELEMENTS: Python sees `t.k` and `t[0]`
  (0-based there: the protocol's indexes start at 0). Its sequence is
  `1..#t`, so a table with holes has a short length.
- A table is INSTANTIABLE if `t.new` is a function, and its type name is its
  metatable's `__name` (set it, or every object is a `table`).
- `invoke` passes `self` only to a method found through the metatable; a
  plain field (a module's function) is called without it. So `obj:m()` and
  `mod.f()` both work from other languages.
- **Every function is iterable**, with the generic `for`'s meaning: Lua's
  iterators are functions (`string.gmatch`, `io.lines`, `coroutine.wrap`).
  `ugly_iter` gives an iterator over it; each step is `f(nil, control)`,
  the first value returned being the next control, until `nil`. So Python's
  `for w in string.gmatch(s, "%a+")` works. Tables aren't iterable (Lua's
  `for` doesn't take one); their elements are.
- `ugly_to_string` is `tostring` (`__tostring`, else `__name` and the
  address): STRING if valid UTF-8, else BYTES.

## Lua as a guest

`ugly_rt_import` and `ugly.module("lua", name)` use `require`, so modules
come from `package.path` and `package.cpath`. `ugly_rt_globals` is `_G`, and
`eval` runs a chunk (`return` its value).

## Calls and options

Lua has no keyword arguments, hints or call contexts: a call into Lua with
any option fails with `UGLY_ERR_NOT_SUPPORTED`. From Lua, options go to
languages that take them:

```lua
local builtins = ugly.module("python", "builtins")
builtins.int("ff", ugly.kw{ base = 16 })                     --> 255
ugly.module("java", "java.lang.String").valueOf(65, ugly.opts{ hint = "double" })   --> "65.0"
```

`ugly.kw{}` sends names **in sorted order**: a table has none. That matters
only to Perl, which receives them as pairs.

## Errors

A foreign error is an `ugly.error` userdata with `e.type`, `e.message`,
`e.language` and `e.payload` (the original exception object). Use `pcall`:

```lua
local ok, e = pcall(json.loads, "{oops")
if not ok and ugly.is_error(e) then print(e.language, e.type) end   --> python  JSONDecodeError
```

An `ugly.error` that escapes Lua goes on unchanged, and `error(e.payload)`
rethrows the original exception. A Lua error going out has the type
`__name` of its metatable, `error` for a string, or the Lua type name, and
its message is the string or the table's `message` field.

## Threads

A Lua state belongs to the thread that created it: calls into it from
another thread fail with `UGLY_ERR_WRONG_THREAD`. Languages that serve every
thread (Python, Java, .NET) are shared, so each thread's Lua state reaches
the same Python.

In HAProxy, `lua-load-per-thread` gives each thread its own state and needs
nothing more. With `lua-load`, one state runs on every thread in turn:
call `ugly.shared_by_threads()` when `core.thread == 0`. See
[examples/haproxy/](../../examples/haproxy/).

## Configuring

Keys: `path` and `cpath` (`package.path`, `package.cpath`), ahead of
`LUA_PATH` and `LUA_CPATH`. From another binding:
`ugly.configure("lua", path="./lua/?.lua")` in Python.

## Pitfalls

- **Nesting depth.** Lua allows about 200 nested C calls, and every call
  through UGLY costs one. Deep recursion that alternates between Lua and
  another language fails with an error at about that depth, well before
  the core's limit of 1000.
- **One Lua per process.** Lua 5.3 and 5.4 export the same symbols, so a
  process has one of them. The binding must match the host's Lua: load
  `binding/5.3/` into a 5.3 host.
- **0-based and 1-based.** Lua code sees 1-based indexes on proxies, and
  other languages see Lua tables 0-based. Don't translate by hand.
- **HAProxy: use `ugly` inside actions.** HAProxy loads Lua on its main
  thread, so with `lua-load-per-thread` a state that used `ugly` at load
  time would belong to the main thread, not the worker running it. With
  `lua-load`, using it at load time works, Python included: HAProxy's
  master starts Python, and the worker it forks inherits a working one.
- **Calls block.** A call into another language is a plain C call: Lua
  can't yield across it, so it suits quick decisions, not I/O.
- **Strings are bytes.** A Lua string that isn't valid UTF-8 arrives
  elsewhere as BYTES (`bytes` in Python), not as a string.
- **`nil` ends a loop.** A foreign iterator yielding a null (Python's
  `None`) ends Lua's `for` there, as it would end any Lua iterator.
- **Any function looks iterable** to other languages, even one that isn't
  an iterator: a Python `for` over it calls it until it returns `nil`,
  possibly forever. Other languages iterate only when their own code loops,
  so this is Lua's meaning, not a surprise the core adds.
