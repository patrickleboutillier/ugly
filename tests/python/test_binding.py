"""The Python binding: a plain python3 process uses Lua through `import ugly` (TAP)."""

import os
import sys

count = 0
failures = 0


built = os.environ.get("UGLY_TEST_LANGS", "").split(",")


def skip(what, lang):
    global count
    count += 1
    print("ok", count, "-", what, f"# SKIP {lang} isn't built", flush=True)


def ok(cond, what):
    global count, failures
    count += 1
    if not cond:
        failures += 1
    print(("ok" if cond else "not ok"), count, "-", what, flush=True)


import ugly

ok(hasattr(ugly, "module") and hasattr(ugly, "eval"), "import ugly in a plain python3 process")

# Options, before the languages start: tests/configured is on no environment search path.
configured = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "configured")
ugly.configure("lua", path=os.path.join(configured, "?.lua"), cpath=["/configured/?.so"])
ugly.configure("js", path=[configured])
ugly.configure("perl5", path=configured)

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

# README's example: Lua's standard library only.
ok(sorted(["banana", "Apple", "cherry"], key=string.lower) == ["Apple", "banana", "cherry"],
   "Python's sorted() calls Lua's string.lower as its key")
ok(string.gsub("hello world", "%w+", str.capitalize) == "Hello World",
   "Lua's string.gsub takes a Python function (a real Lua function in Lua)")

# Threads: a Python thread gets its own ctx and its own Lua; Lua values stay on their thread.
import threading

seen = {}


def work():
    try:
        seen["lua"] = ugly.module("lua", "string").upper("abc")
    except Exception as e:
        seen["lua"] = e
    try:
        string.upper("x")      # the main thread's Lua
    except Exception as e:
        seen["other"] = e


t = threading.Thread(target=work)
t.start()
t.join()
ok(seen.get("lua") == "ABC", f"a Python thread gets its own ctx and Lua runtime: {seen.get('lua')!r}")
ok(isinstance(seen.get("other"), ugly.Error) and "another thread" in seen["other"].message,
   f"  ... and the main thread's Lua values stay the main thread's: {seen.get('other')!r}")

# Foreign functions are Lua functions; classes stay userdata.
nums = ugly.eval("lua", "return { 3, 1, 2 }")
ugly.module("lua", "table").sort(nums, lambda a, b: a > b)
ok(list(nums) == [3, 2, 1], "table.sort takes a Python comparison function")
probe = ugly.eval("lua", "return function(x) return type(x) .. ' ' .. tostring(require('ugly').is_proxy(x)) end")
ok(probe(len) == "function true", "a Python function is a Lua function, and ugly.is_proxy knows it")
ok(probe(dict) == "userdata true", "a Python class stays userdata (its members stay reachable)")
same = ugly.eval("lua", "return function(f) return f end")
ok(same(len) is len, "a Python function round trip through Lua gives the original")
t = ugly.module("lua", "os").date("!*t", 0)
ok((t.year, t.month) == (1970, 1), "os.date('!*t', 0) gives a Lua table: t.year, t.month")
squares = ugly.eval("lua", "local t = {} for i = 1, 4 do t[i] = i * i end return t")
ok(list(squares) == [1, 4, 9, 16], "a Lua table as a Python sequence: list(squares)")

# Lua code using Python through its own ugly module, reaching back into this very interpreter.
ok(ugly.eval("lua", "return require('ugly').module('python', 'math').sqrt(16)") == 4.0,
   "embedded Lua uses require('ugly') to reach this process's Python")
ok(ugly.eval("lua", "return require('ugly').module('python', '__main__').obj") is obj,
   "  ... and sees this script's own globals")

# Options took effect, and can't change once a language has started.
ok(ugly.module("lua", "ugly_configured").where == "configured", "configure('lua', path=...): Lua finds the module")
ok(ugly.eval("lua", "return package.cpath").startswith("/configured/?.so;"), "  ... and cpath comes first in package.cpath")
if "js" in built:
    ok(ugly.module("js", "ugly_configured").where == "configured", "configure('js', path=...): JS finds the module")
else:
    skip("configure('js', path=...)", "js")
if "perl5" in built:
    ok(ugly.module("perl5", "ugly_configured").where() == "configured", "configure('perl5', path=...): Perl finds the module")
else:
    skip("configure('perl5', path=...)", "perl5")
try:
    ugly.configure("lua", path="/too/late/?.lua")
    ok(False, "configure after the language started raises ugly.Error")
except ugly.Error as e:
    ok("already started" in e.message, "configure after the language started raises ugly.Error")
try:
    ugly.configure("js", path=42)
    ok(False, "configure with a value that isn't a string raises TypeError")
except TypeError:
    ok(True, "configure with a value that isn't a string raises TypeError")

print(f"1..{count}")
sys.exit(1 if failures else 0)
