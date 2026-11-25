-- HAProxy asks Python for a decision on each request (lua-load or
-- lua-load-per-thread this file). examples/haproxy/policy.py is plain Python.
--
-- Use ugly in actions, not when the file loads: HAProxy loads Lua on its main
-- thread (and in its master process, before forking), then runs actions on
-- the workers. The binding attaches on first use, so this state belongs to
-- the worker that runs it, and Python starts there.

local ugly = require "ugly"
local policy

-- With lua-load, HAProxy runs this one state on all its threads, one at a
-- time (core.thread is 0); with lua-load-per-thread, each thread has its own.
if core.thread == 0 then
    ugly.shared_by_threads()
end

core.register_action("decide", { "http-req" }, function(txn)
    local ok, verdict = pcall(function()
        policy = policy or ugly.module("python", "policy")
        -- A Python tuple, (verdict, reason).
        return policy.decide(txn.sf:src(), txn.sf:path(), txn.sf:req_hdr("user-agent"))
    end)
    if not ok then
        txn:set_var("txn.verdict", "deny")                -- fail closed
        txn:set_var("txn.reason", "policy error: " .. tostring(verdict))
        return
    end
    txn:set_var("txn.verdict", verdict[1])
    txn:set_var("txn.reason", verdict[2])
end)
