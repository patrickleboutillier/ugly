-- Python started by UGLY is never finalized, which is what flushes its
-- buffered output: what it printed must still reach a pipe (as this test's
-- output is) when the process ends. The second TAP line is Python's.

local ugly = require "ugly"
print("1..2")
print("ok 1 - a Lua script that makes Python print")
io.stdout:flush()
ugly.eval("python", "print('ok 2 - what Python printed reaches a pipe at exit')")
os.exit(0, true)
