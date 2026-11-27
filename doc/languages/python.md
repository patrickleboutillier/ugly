# Python

CPython 3.10 or later, language `"python"`. How Python maps onto the
protocol is the CPython column of [the contract's §10](../contract.md#10-the-languages);
this page is the practical side.

## From Python: the binding

The binding is the extension module `ugly`, built against the Python Meson
found (`plugins/python/binding/`, installed into site-packages). Put its
directory on `PYTHONPATH` if it isn't installed, and run it with that same
Python:

```sh
PYTHONPATH=build/plugins/python/binding python3 script.py
```

```python
import ugly

string = ugly.module("lua", "string")              # a module of another language
string.upper("abc")                                 # 'ABC'
sorted(["banana", "Apple"], key=string.lower)       # Python calls a Lua function

squares = ugly.eval("lua", "local t = {} for i = 1, 3 do t[i] = i * i end return t")
list(squares)                                       # [1, 4, 9]
```

| Name | What it is |
|---|---|
| `ugly.module(lang, name)` | imports a module of `lang` in this thread's runtime of it |
| `ugly.eval(lang, code)` | evaluates code in a language that can (`UGLY_CAP_EVAL`) |
| `ugly.configure(lang, key=value or [values])` | options for a language that hasn't started ([Configuring](#configuring)) |
| `ugly.opts(hint=..., context=...)` | call options other than keyword arguments, passed last |
| `ugly.Proxy` | the type of every foreign value |
| `ugly.Error` | an error from another language: `.type`, `.message`, `.language`, `.payload` |
| `ugly.rethrow(e)` | raises a foreign exception object again, keeping its identity |

The binding attaches to the running interpreter at import, and tears down
at exit (`atexit`).

## Foreign values in Python

A foreign value is an `ugly.Proxy`, one per value (`==` is identity, and
proxies hash).

- `p.x`, `p.x = v`, `del p.x`: members. `p[0]`, `p[i] = v`, `len(p)`,
  `list(p)`: elements, 0-based. `p["k"]` reads a member by a name that
  isn't an identifier, and `del p["k"]` removes it.
- **A foreign keyed collection** (a Lua table, a Perl hash, a `Map`) works
  as a dict does: `p[key]` (a `KeyError` if it isn't there), `p[key] = v`,
  `del p[key]`, `key in p`, `len(p)`, and `keys()`, `values()`, `items()`
  and `get(key, default)`, as lists of what is there now, unless the value
  has a member of that name itself. Looping over one its language doesn't
  iterate gives its keys. Python's syntax tells an entry from a member, so
  nothing falls back: `p["get"]` is the entry, `p.get` the member.
- A foreign sequence works as a list does: `p[-1]` counts from the end,
  `p[1:3]` is a Python list of those elements, `p[1:3] = items`,
  `del p[1:3]` and `del p[i]` resize it (a slice with a step can be read,
  not assigned), and `append`, `extend`, `insert`, `pop` and `clear` are
  there unless the value has a member of that name itself, which comes
  first (a JS array's own `pop`). A sequence its language can't resize
  refuses (a Haskell list, a Java array).
- `p.m(x)` invokes `m` with the foreign language's receiver rules. A member
  that is a class stays the class, so `mod.Point.origin` is its member; one
  that is callable too (a JS function, which is also a constructor) is
  still called on its owner, so `this` is right.
- `bool(p)` is false for an empty sequence, as Python's own are, and true
  for everything else. It asks for the first element, not the length, so
  an infinite Haskell list is true at once. A keyed collection is false
  when it has no entries, so a Lua table with only named fields is true.
- `p(x)` calls it; calling a class that can only be instantiated (a Java,
  Perl or JS class) constructs it: `perl.KwBox(value=5)`.
- **Iteration is the foreign language's own** (contract §2): `for x in p`,
  `list(p)`, `iter(p)` and `next(p)` run what that language's loop runs,
  one value per step, so a generator runs only as far as Python's loop
  goes. A Lua iterator function works as is: `for w in
  string.gmatch(s, "%a+")`. A value its language doesn't iterate but that
  has elements (a Lua table) gets Python's sequence protocol, as any object
  with `__getitem__` does: `list(squares)`. An error raised by the foreign
  iterator ends the loop as `ugly.Error`, after the values before it.
- `str(p)` is the foreign language's stringification (Lua's `tostring`,
  Java's `toString()`); `repr(p)` stays `<ugly.Proxy type>`.
- **Keyword arguments are native**: `json.dumps(data, indent=2)` into
  Python, `perl.kw(1, c=3)` into Perl. A language without them (Lua, JS,
  Java) refuses the call rather than drop them.

## Python values elsewhere

| Python | goes out as | comes back as |
|---|---|---|
| `None` | NULL | `None` |
| `bool` | BOOL | `bool` |
| `int` | INT (`UGLY_ERR_TYPE` beyond int64) | `int` |
| `float` | DOUBLE | `float` |
| `str` / `bytes` | STRING / BYTES | `str` / `bytes` |
| anything else | a handle | the original object |

- Members are attributes, a dict's too (its methods). A dict's keys are its
  **entries** (contract §2): any mapping has them (a dict, an
  `OrderedDict`, `os.environ`), with any hashable key, compared as Python
  compares keys (`1`, `1.0` and `True` are one). Elements are the sequence
  protocol (lists, tuples...).
- Iterable is what `iter()` takes: lists, dicts (their keys), generators,
  files, anything with `__iter__` or the sequence protocol. `ugly_iter` on
  an iterator is that iterator; each step is `next()`, and a generator runs
  only as far as asked. Releasing a generator early closes it (its
  `finally` runs).
- `ugly_to_string` is `str()`.
- `invoke` calls the bound method, so `self` is always right.
- Conversions are Python's own, strict ones: asking for INT from `"42"` or
  from `2.0` fails with `UGLY_ERR_TYPE`; an int widens to DOUBLE.

## Python as a guest

`import` finds modules on `sys.path` (`PYTHONPATH`, or the `path` option).
`ugly_rt_globals` is the runtime's globals (`__main__`). `eval` gives an
expression's value, or runs statements and gives `None`.

## Errors

An error from another language is raised as `ugly.Error`; if it escapes
Python unhandled, it goes on unchanged, as the original error.

```python
try:
    ugly.module("lua", "m").fails()
except ugly.Error as e:
    e.language, e.type, e.message, e.payload       # payload: the original error object
```

A Python exception going out has its class name as the type, and is itself
the payload. When it comes back to Python it is raised as the same object.
To raise a foreign exception object again (one you got as a payload),
use `ugly.rethrow(e)`.

## Threads

Python serves every thread (`UGLY_CAP_ANY_THREAD`): there is one Python per
process, shared by every thread that uses it, and the GIL serializes the
Python work. Python threads that call other languages get a context of their
own automatically, and Python releases the GIL while a call into another
language runs.

## Configuring

Key: `path` (`sys.path`), ahead of `PYTHONPATH`:
`ugly.configure("python", path=["./py"])`, or `ugly_configure` in C, before
Python starts.

## Pitfalls

- **One Python per process, for good.** Python starts at plugin load and is
  never finalized, so there's no second runtime and no restart. Its memory
  shows as "still reachable" at exit. What it printed is flushed when its
  runtime is deleted and when the process exits, so output sent to a pipe
  or a file arrives, after the host's own if the host flushed first.
- **No signal handlers.** The plugin starts Python with
  `Py_InitializeEx(0)`, so Ctrl-C doesn't raise `KeyboardInterrupt` in
  embedded Python: signals belong to the host.
- **Forking after Python started works**, even from C: the plugin tells
  CPython about every `fork()` in the process (`pthread_atfork`), the way
  `os.fork()` does, so each child has a working interpreter (HAProxy's
  master starting its workers). The child has only the thread that forked;
  Python threads the parent had are gone, as with `os.fork()`.
- **Every proxy has `__next__`**, so `isinstance(p, collections.abc.Iterator)`
  is true of any proxy; `next(p)` on one that isn't a foreign iterator
  raises `TypeError`.
- **Foreign elements can't be deleted or resized from Python** yet:
  `del p[0]`, `p.append(x)` and slice assignment aren't there (TODO).
  Assign to `p[len(p)]` to append to a Lua table or a Java list.
- **Same interpreter as the build.** The binding is built against one
  Python; load it into that one. Debian's `python3` is statically linked,
  so the binding links no `libpython`.
