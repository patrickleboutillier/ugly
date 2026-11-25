"""The decision HAProxy asks Python for, through UGLY (examples/haproxy/poc.lua).

An ordinary Python module: it knows nothing about HAProxy or Lua.
"""

import ipaddress

BLOCKED_AGENTS = ("badbot", "scanner")
ADMIN_NETWORK = ipaddress.ip_network("10.0.0.0/8")


def decide(ip, path, agent):
    """'allow' or 'deny' for a request, and why."""
    agent = (agent or "").lower()
    if any(bad in agent for bad in BLOCKED_AGENTS):
        return "deny", "blocked user agent"
    if path.startswith("/admin") and ipaddress.ip_address(ip) not in ADMIN_NETWORK:
        return "deny", "admin from outside " + str(ADMIN_NETWORK)
    return "allow", "ok"
