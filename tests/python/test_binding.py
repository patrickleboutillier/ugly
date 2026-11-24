"""The Python binding: a plain python3 process uses Lua through `import ugly` (TAP)."""

import sys

count = 0
failures = 0


def ok(cond, what):
    global count, failures
    count += 1
    if not cond:
        failures += 1
    print(("ok" if cond else "not ok"), count, "-", what, flush=True)


import ugly

ok(hasattr(ugly, "module") and hasattr(ugly, "eval"), "import ugly in a plain python3 process")

lua = ugly.module("lua", "conformance")
ok(isinstance(lua, ugly.Proxy), "ugly.module('lua', ...) gives a Lua module, as a proxy")
ok(lua.echo("héllo") == "héllo", "a string goes to Lua and back")
ok(lua.apply(lambda x: x * 2, 21) == 42, "Lua calls a Python lambda")

c = lua.make_counter()
c.inc()
ok(c.inc() == 2, "a method call on a Lua object: c.inc()")
ok(c.count == 2, "a member of a Lua object: c.count")
ok(len(c.items) == 2 and list(c.items) == [1, 2], "a Lua list: len() and iteration")
ok(lua.echo(c) is c, "a Lua object round trip gives the same proxy")
obj = object()
ok(lua.echo(obj) is obj, "a Python object round trip gives the original")

try:
    lua.raise_error("boom")
    ok(False, "a Lua error raises ugly.Error")
except ugly.Error as e:
    ok(e.type == "ConformanceError" and e.message == "boom" and e.language == "lua",
       "a Lua error raises ugly.Error, with type, message and language")


class Mine(Exception):
    pass


mine = Mine("mine")


def raiser(_):
    raise mine


try:
    lua.apply(raiser, 1)
    ok(False, "a Python exception crossing Lua")
except Mine as e:
    ok(e is mine, "a Python exception crossing Lua comes back as the same object")

ok(ugly.eval("lua", "return 6 * 7") == 42, "ugly.eval('lua', code)")
string = ugly.module("lua", "string")
ok(string.upper("abc") == "ABC", "Lua's string library: string.upper from Python")

# Lua code using Python through its own ugly module, reaching back into this very interpreter.
ok(ugly.eval("lua", "return require('ugly').module('python', 'math').sqrt(16)") == 4.0,
   "embedded Lua uses require('ugly') to reach this process's Python")
ok(ugly.eval("lua", "return require('ugly').module('python', '__main__').obj") is obj,
   "  ... and sees this script's own globals")

print(f"1..{count}")
sys.exit(1 if failures else 0)
