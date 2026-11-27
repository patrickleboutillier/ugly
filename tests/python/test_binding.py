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

# Keyword arguments (contract §11): native on proxies; Lua has none and refuses.
try:
    string.upper("x", key=1)
    ok(False, "keyword arguments to Lua are refused")
except ugly.Error as e:
    ok("takes no call options" in e.message, "keyword arguments to Lua are refused (ugly.Error)")
if "perl5" in built:
    pl = ugly.module("perl5", "conformance")
    ok(pl.kw(1, c=3, b=2) == "1 2 3", "keyword arguments reach Perl as trailing pairs: kw(1, c=3, b=2)")
    ok(pl.KwBox(value=5).add(n=10) == 15, "calling a Perl class constructs it, with keyword arguments: KwBox(value=5).add(n=10)")
    ok((pl.context(), pl.context(ugly.opts(context="scalar"))) == ("list", "scalar"),
       "ugly.opts(context=\"scalar\") last: Perl's scalar context")
    localtime = ugly.eval("perl5", "sub { localtime($_[0]) }")
    ok(len(localtime(0)) == 9 and isinstance(localtime(0, ugly.opts(context="scalar")), str),
       "  ... localtime(0): 9 elements in list context, a date string in scalar (README)")
else:
    skip("keyword arguments into Perl", "perl5")
    skip("calling a Perl class", "perl5")
    skip("Perl's call context", "perl5")
    skip("Perl's localtime in scalar context", "perl5")

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

# Iteration (contract §2): Lua's iterators are functions, and Python's for takes them.
words = [w for w in string.gmatch("one two three", "%a+")]
ok(words == ["one", "two", "three"], "for w in string.gmatch(...): a Lua iterator in a Python loop")
it = iter(string.gmatch("a b", "%a+"))
ok(iter(it) is it and next(it) == "a" and next(it) == "b" and next(it, None) is None,
   "  ... iter() is the iterator itself; next() until StopIteration")
ok(lua.sum_iter(x * x for x in range(4)) == 14, "Lua's for over a Python generator: sum_iter(...)")
ok(list(lua.count_to(3)) == [0, 1, 2], "list() of a Lua iterator function")
try:
    list(lua.fail_after(1))
    ok(False, "a Lua iterator's error reaches Python's loop")
except ugly.Error as e:
    ok(e.type == "ConformanceError", "a Lua iterator's error reaches Python's loop")
ok(str(c) == "counter(2)" and repr(c) == "<ugly.Proxy counter>", "str(c) is Lua's tostring(c); repr() stays the proxy's")
try:
    iter(ugly.module("lua", "io").stdout)
    ok(False, "iter() of a Lua value neither iterable nor with elements (a file): TypeError")
except TypeError:
    ok(True, "iter() of a Lua value neither iterable nor with elements (a file): TypeError")

# Lua code using Python through its own ugly module, reaching back into this very interpreter.
ok(ugly.eval("lua", "return require('ugly').module('python', 'math').sqrt(16)") == 4.0,
   "embedded Lua uses require('ugly') to reach this process's Python")
ok(ugly.eval("lua", "return require('ugly').module('python', '__main__').obj") is obj,
   "  ... and sees this script's own globals")

# More arguments than Lua's stack has room for unasked: the Lua plugin asks for the room.
count_args = ugly.eval("lua", "return function(...) return select('#', ...) end")
ok(count_args(*range(1000)) == 1000, "a Lua function called with 1000 arguments gets them all")
taker = ugly.eval("lua", "return { m = function(...) return select('#', ...) end }")
ok(taker.m(*range(300)) == 300, "  ... and a Lua method invoked with 300")
try:
    count_args(*range(2000000))
    ok(False, "  ... and more than Lua's stack can hold raises ugly.Error")
except ugly.Error as e:
    ok("stack overflow" in e.message, f"  ... and more than Lua's stack can hold raises ugly.Error: {e.message}")

# Assigning through a proxy: the key is read before the GIL is released (-1 is CPython's error value).
t = ugly.eval("lua", "return { 1, 2, 3, name = 'x' }")
t[1] = 20
t["name"] = "y"
ok(t[1] == 20 and t["name"] == "y", "proxy[int] = v and proxy[str] = v")
try:
    t[-1] = 5
    ok(False, "proxy[-1] = v is refused by Lua")
except ugly.Error as e:
    ok(True, f"proxy[-1] = v is refused by Lua: {e.message}")
class BadIndex:
    def __index__(self):
        raise ValueError("no index")
for key, exc, what in ((BadIndex(), ValueError, "an __index__ that raises"), (1.5, TypeError, "a float key"),
        ("\udc80", UnicodeEncodeError, "a key that isn't UTF-8")):
    try:
        t[key] = 1
        ok(False, f"proxy[key] = v with {what} raises {exc.__name__}")
    except exc:
        ok(True, f"proxy[key] = v with {what} raises {exc.__name__}")

# A program that only calls out: what it passed is freed at its next use of ugly, not kept
# until something calls into Python (releases to Python are queued, and the binding drains them).
import gc
import weakref
class Passed:
    pass
def pass_one():
    o = Passed()
    lua.echo(o)
    return weakref.ref(o)
passed = [pass_one() for _ in range(100)]
ugly.eval("lua", "collectgarbage() collectgarbage()")     # Lua drops its proxies of them
lua.echo(1)
gc.collect()
ok(not any(w() for w in passed), "Python objects passed to Lua are freed at the next use of ugly")

if "perl5" in built:
    # Perl called from a language that isn't Perl: its temporaries end with each call.
    lu = ugly.module("perl5", "List::Util")
    refs = ugly.eval("perl5", "require B; sub { B::svref_2object(\\&List::Util::max)->REFCNT }")
    before = refs()
    for _ in range(100):
        lu.max(1, 2)
    ok(refs() == before, f"100 calls of a Perl package's sub leave no reference to it: {refs()} == {before}")
    ugly.eval("perl5", "package Counted; our $gone = 0; sub new { bless [], shift } sub DESTROY { $gone++ } 1")
    arr = ugly.eval("perl5", "[ map { Counted->new } 1 .. 3 ]")
    n = len(list(arr))
    del arr
    gc.collect()
    gone = ugly.eval("perl5", "$Counted::gone")
    ok(n == 3 and gone == 3, f"a Perl array Python iterated is destroyed when dropped: {gone} of 3")
else:
    skip("Perl's temporaries, calls", "perl5")
    skip("Perl's temporaries, iteration", "perl5")

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
