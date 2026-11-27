-- The conformance fixtures (doc/contract.md section 9), in Lua.
--
-- Type names the suite checks (describe) come from each metatable's __name:
-- module, counter, list, class, point. Exceptions are ConformanceError.

local M = setmetatable({}, { __name = "module" })

-- Objects whose finalization the suite counts: counters and their item lists.
local finalized = 0
local function tracked(t, name, index, tostr)
	return setmetatable(t, {
		__name = name,
		__index = index,
		__tostring = tostr,
		__gc = function() finalized = finalized + 1 end,
	})
end

local is_proxy = require("ugly").is_proxy
local iter = require("ugly").iter            -- for x in iter(xs): a foreign iterable, or a Lua function     -- userdata, or a function for a foreign function

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
	return tracked({ count = 0, items = tracked({}, "list") }, "counter", Counter,
		function(c) return "counter(" .. c.count .. ")" end)
end

-- Benchmarks (tests/bench.c): f(i) n times, and our own echo n times.
function M.bench(f, n)
    for i = 1, n do f(i) end
    return n
end

function M.bench_self(n)
    local echo = M.echo
    for i = 1, n do echo(i) end
    return n
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

-- Iteration (contract §2): Lua's iterators are functions, called until nil.
local produced = 0

function M.count_to(n)
	local i = 0
	produced = 0
	return function()
		if i < n then
			i = i + 1
			produced = produced + 1
			return i - 1
		end
	end
end

function M.naturals()
	local i = -1
	local anchor = tracked({}, "anchor")     -- collected with the function
	return function()
		local _ = anchor
		i = i + 1
		return i
	end
end

function M.fail_after(n)
	local i = 0
	return function()
		if i < n then
			i = i + 1
			return i - 1
		end
		error(setmetatable({ message = "fail_after(" .. n .. ")" }, ConformanceError))
	end
end

function M.produced()
	return produced
end

function M.sum_iter(xs)
	local total = 0
	for x in iter(xs) do
		total = total + x
	end
	return total
end

function M.take_iter(xs, n)
	local t = {}
	if n > 0 then
		for x in iter(xs) do
			t[#t + 1] = x
			if #t >= n then break end
		end
	end
	return t
end

function M.stringify(x)
	return tostring(x)
end

return M
