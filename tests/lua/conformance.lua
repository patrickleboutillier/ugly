-- The conformance fixtures (doc/v2/contract.md section 9), in Lua.
--
-- Type names the suite checks (describe) come from each metatable's __name:
-- module, counter, list, class, point. Exceptions are ConformanceError.

local M = setmetatable({}, { __name = "module" })

-- Objects whose finalization the suite counts: counters and their item lists.
local finalized = 0
local function tracked(t, name, index)
	return setmetatable(t, {
		__name = name,
		__index = index,
		__gc = function() finalized = finalized + 1 end,
	})
end

local function is_proxy(x)
	local mt = getmetatable(x)
	return type(x) == "userdata" and type(mt) == "table" and mt.__name == "ugly.proxy"
end

local function is_ugly_error(x)
	local mt = getmetatable(x)
	return type(x) == "userdata" and type(mt) == "table" and mt.__name == "ugly.error"
end


function M.echo(x)
	return x
end

function M.kind_of(x)
	local t = type(x)
	if t == "nil" then return "null" end
	if t == "boolean" then return "bool" end
	if t == "number" then return math.type(x) == "integer" and "int" or "double" end
	if t == "string" then return utf8.len(x) and "string" or "bytes" end
	if is_proxy(x) then return "proxy" end
	local mt = getmetatable(x)
	return (type(mt) == "table" and mt.__name) or t
end

local Counter = {}
function Counter.inc(self)
	self.count = self.count + 1
	self.items[#self.items + 1] = self.count
	return self.count
end

function M.make_counter()
	return tracked({ count = 0, items = tracked({}, "list") }, "counter", Counter)
end

function M.apply(f, x)
	return f(x)
end

function M.call_twice(f, x)
	return f(f(x))
end

local ConformanceError = { __name = "ConformanceError" }

function M.raise_error(msg)
	error(setmetatable({ message = msg }, ConformanceError))
end

-- Returns the exception object: ours as is, a foreign one through its payload.
function M.catch_and_return(f)
	local ok, e = pcall(f)
	if ok then return e end
	if is_ugly_error(e) then return e.payload end
	return e
end

function M.rethrow(e)
	error(e)
end

function M.finalized()
	collectgarbage("collect")
	collectgarbage("collect")
	return finalized
end

local Point = setmetatable({}, { __name = "class" })
local PointMT = { __name = "point", __index = {} }
function PointMT.__index.norm2(p)
	return p.x * p.x + p.y * p.y
end
function Point.new(x, y)
	return setmetatable({ x = x, y = y }, PointMT)
end
M.Point = Point

return M
