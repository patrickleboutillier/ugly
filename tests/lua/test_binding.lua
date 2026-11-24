-- The Lua binding: a plain lua process uses Python through require "ugly" (TAP).

local count, failures = 0, 0
local function ok(cond, what)
	count = count + 1
	if not cond then failures = failures + 1 end
	print((cond and "ok " or "not ok ") .. count .. " - " .. what)
end

local ugly = require "ugly"
ok(type(ugly.module) == "function" and type(ugly.eval) == "function", 'require "ugly" in a plain lua process')

local py = ugly.module("python", "conformance")
ok(ugly.is_proxy(py), "ugly.module('python', ...) gives a Python module, as a proxy")
ok(py.echo("h\u{e9}llo") == "h\u{e9}llo", "a string goes to Python and back")
ok(py.apply(function(x) return x * 2 end, 21) == 42, "Python calls a Lua function")

local c = py.make_counter()
c:inc()
ok(c:inc() == 2, "a method call on a Python object: c:inc()")
ok(c.inc() == 3, "  ... and c.inc()")
ok(c.count == 3, "a member of a Python object: c.count")
ok(#c.items == 3 and c.items[1] == 1 and c.items[3] == 3, "a Python list: # and 1-based indexing")
ok(rawequal(py.echo(c), c), "a Python object round trip gives the same proxy")
local t = {}
ok(rawequal(py.echo(t), t), "a Lua table round trip gives the original")

local ok1, e = pcall(py.raise_error, "boom")
ok(not ok1 and ugly.is_error(e) and e.type == "ConformanceError" and e.message == "boom" and e.language == "python",
	"a Python error is an ugly error, with type, message and language")

local mine = setmetatable({}, { __name = "Mine" })
local ok2, e2 = pcall(py.apply, function() error(mine) end, 1)
ok(not ok2 and rawequal(e2, mine), "a Lua error crossing Python comes back as the same value")

local pymath = ugly.module("python", "math")
ok(math.abs(pymath.sqrt(2) - 1.4142135623731) < 1e-9, "Python's math.sqrt from Lua")
ok(ugly.eval("python", "6 * 7") == 42, "ugly.eval('python', code)")

-- Python code using Lua through its own ugly module, reaching back into this very interpreter.
ok(ugly.eval("python", "__import__('ugly').module('lua', 'string').upper('abc')") == "ABC",
	"embedded Python uses import ugly to reach this process's Lua")
shared = { answer = 42 }
ok(ugly.eval("python", "__import__('ugly').module('lua', '_G').shared.answer") == 42,
	"  ... and sees this script's own globals")

print("1.." .. count)
os.exit(failures == 0 and 0 or 1, true)      -- true: close the state, so the binding tears down
