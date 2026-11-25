"""The HAProxy proof of concept, as a test (TAP): HAProxy runs Lua, which asks
Python (policy.py) whether to let each request through.

    run_poc.py HAPROXY_BIN [NBTHREAD [lua-load|lua-load-per-thread]]

It needs the environment the tests set (UGLY_PLUGIN_PATH, and LUA_CPATH to
the Lua binding that matches HAProxy's Lua); it adds this directory to
PYTHONPATH for policy.py.
"""

import http.client
import os
import socket
import subprocess
import sys
import tempfile
import time

here = os.path.dirname(os.path.abspath(__file__))
haproxy = sys.argv[1]
nbthread = sys.argv[2] if len(sys.argv) > 2 else "1"
load = sys.argv[3] if len(sys.argv) > 3 else "lua-load"

count = 0
failures = 0


def ok(cond, what):
    global count, failures
    count += 1
    if not cond:
        failures += 1
    print(("ok" if cond else "not ok"), count, "-", what, flush=True)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def request(port, path, agent="poc-test"):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    conn.request("GET", path, headers={"User-Agent": agent})
    r = conn.getresponse()
    body = r.read().decode()
    conn.close()
    return r.status, body.strip()


port = free_port()
with open(os.path.join(here, "haproxy.cfg")) as f:
    cfg = f.read()
cfg = (cfg.replace("@PORT@", str(port)).replace("@NBTHREAD@", nbthread)
       .replace("@LUA@", f"{load} {os.path.join(here, 'poc.lua')}"))

env = dict(os.environ)
env["PYTHONPATH"] = here + os.pathsep + env.get("PYTHONPATH", "")

with tempfile.TemporaryDirectory() as tmp:
    cfg_path = os.path.join(tmp, "haproxy.cfg")
    with open(cfg_path, "w") as f:
        f.write(cfg)
    # -db: foreground, no fork; HAProxy's own output goes to stderr.
    proc = subprocess.Popen([haproxy, "-db", "-f", cfg_path], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        for _ in range(100):
            if proc.poll() is not None:
                break
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.1).close()
                break
            except OSError:
                time.sleep(0.05)
        ok(proc.poll() is None, f"haproxy is up ({load}, nbthread {nbthread})")

        status, body = request(port, "/")
        ok(status == 200 and body == "allowed: ok", f"GET / is allowed by Python: {status} {body!r}")
        status, body = request(port, "/admin")
        ok(status == 403 and "admin from outside" in body, f"GET /admin from 127.0.0.1 is denied by Python: {status} {body!r}")
        status, body = request(port, "/", agent="BadBot/1.0")
        ok(status == 403 and "blocked user agent" in body, f"a blocked user agent is denied: {status} {body!r}")

        # Many requests, over separate connections, so that with several
        # threads they land on different ones.
        results = [request(port, "/page%d" % i)[0] for i in range(60)]
        ok(results.count(200) == 60, f"60 more requests, all allowed: {sorted(set(results))}")
    finally:
        proc.terminate()
        try:
            _, err = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            _, err = proc.communicate()
        for line in err.splitlines():
            print("# haproxy: " + line)

print(f"1..{count}")
sys.exit(1 if failures else 0)
