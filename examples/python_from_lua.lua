-- A plain Lua script using Python's standard library, through require "ugly".
-- Run by `meson test` (examples suite); the asserts make it a test.

local ugly = require "ugly"
local python = function(name) return ugly.module("python", name) end

local json, statistics = python "json", python "statistics"
local functools, datetime = python "functools", python "datetime"

-- Python parses JSON; Lua reads the dict and the list it gives back.
local data = json.loads('{"name": "ugly", "scores": [3, 1, 4, 1, 5]}')
print(data.name, #data.scores, data.scores[1])     --> ugly    5    3

-- Python computes on its own list...
local mean = statistics.mean(data.scores)
print(mean)                                         --> 2.8

-- ...and on a Lua table, calling back into a Lua function.
local product = functools.reduce(function(a, b) return a * b end, { 1, 2, 3, 4, 5 })
print(product)                                      --> 120

-- Python objects have methods.
local day = datetime.date(2026, 11, 24)
print(day:isoformat(), day.year)                    --> 2026-11-24    2026

-- Python exceptions arrive as errors, with their type and message.
local ok, err = pcall(json.loads, "{oops")
print(ok, err.type)                                 --> false   JSONDecodeError

assert(data.name == "ugly" and #data.scores == 5 and data.scores[1] == 3)
assert(mean == 2.8 and product == 120)
assert(day:isoformat() == "2026-11-24" and day.year == 2026)
assert(not ok and ugly.is_error(err) and err.type == "JSONDecodeError" and err.language == "python")
