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

- `p.x`, `p.x = v`: members. `p[0]`, `p[i] = v`, `len(p)`, `list(p)`:
  elements, 0-based. `p["k"]` reads a member by a name that isn't an
  identifier.
- `p.m(x)` invokes `m` with the foreign language's receiver rules. A member
  that is a class stays the class.
- `p(x)` calls it; calling a class that can only be instantiated (a Java or
  Perl class) constructs it: `perl.KwBox(value=5)`.
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

- Members are attributes, or the keys of a dict. Elements are the sequence
  protocol (lists, tuples...).
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
  shows as "still reachable" at exit.
- **No signal handlers.** The plugin starts Python with
  `Py_InitializeEx(0)`, so Ctrl-C doesn't raise `KeyboardInterrupt` in
  embedded Python: signals belong to the host.
- **Forking after Python started.** A host that forks after Python started
  (HAProxy's master, if Python is used at load time) gives every child a
  copy of an interpreter with its threads gone. Start Python in the child.
  Not detected yet (TODO).
- **Foreign elements can't be deleted or resized from Python** yet:
  `del p[0]`, `p.append(x)` and slice assignment aren't there (TODO).
  Assign to `p[len(p)]` to append to a Lua table or a Java list.
- **Same interpreter as the build.** The binding is built against one
  Python; load it into that one. Debian's `python3` is statically linked,
  so the binding links no `libpython`.
