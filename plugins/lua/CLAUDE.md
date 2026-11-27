# The Lua plugin

Notes for Claude Code, loaded when working in this directory. Shared rules: `plugins/CLAUDE.md` and the root `CLAUDE.md`.

**`lua/`** is Lua 5.4 or 5.3 (`-Dlua_version=auto|5.4|5.3`; one Lua per process, language `"lua"` either way). The binding is built for every Lua found, in `binding/<version>/` (identical meson files); tests are `binding-lua5.4` and `binding-lua5.3`. A `LUA_VERSION_NUM` shim covers 5.3.

- Every operation runs under `lua_pcall` (`op_body`), except `call`, which pcalls the target directly so a call costs one of Lua's roughly 200 nested C calls. Lua doesn't check pushes and guarantees a C function 20 slots: `lua_checkstack` (`ARG_SLOTS`) before pushing a call's arguments.
- Proxies are `ugly.proxy` userdata, cached per owned handle in a weak table. A foreign function (CALLABLE, not INSTANTIABLE) is a C closure (`proxy_fn`) over that userdata instead, so Lua's C library takes it; `to_proxy()` recognizes both. A callable member that isn't a class becomes a method closure (`proxy_method`); a class member stays the class. Foreign errors travel as `ugly.error` userdata, with `e.payload`, and are restored unchanged when they escape.
- A member that is CALLABLE and INSTANTIABLE (a Python class, a JS function) is a proxy userdata of its own (not the cached one) with `{ owner, name }` as its user value: `proxy_call` invokes the member on the owner.
- `ugly.tostring(v)` is `ugly_to_string` for a foreign function (a closure: Lua's `tostring` of it can't be ours). An element's index is an integer or a float with an integer's value (`index_key`).
- Elements are 0-based in the protocol and 1-based in Lua.
- `invoke` passes `self` only to methods found through the metatable, not to raw fields.
