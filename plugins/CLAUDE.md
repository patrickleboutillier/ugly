# Plugins and bindings

Notes for Claude Code, loaded when working in this directory. The project's rules are in the root `CLAUDE.md`. Each language has its own notes in `plugins/<lang>/CLAUDE.md`.

**`plugins/<lang>/`** holds the plugins, built as `ugly_<lang>.so` and exporting `ugly_plugin_<lang>()`. Each one's `meson.build` appends to `plugins`, `plugin_dirs` and `test_plugins`, and is optional (`-D<lang>=`).

- Proxies in all six languages turn a callable member into `ugly_invoke`, so `obj.m(x)` uses the foreign language's receiver rules.

- Rule 7 is complete: an error whose payload comes home is raised as the original object (`throw_ugly_error`, `raise_ugly`).

## Bindings

**Bindings** (`plugins/<lang>/binding/`) compile the plugin's own source again with `-DUGLY_BINDING`, as the native module `ugly`: `luaopen_ugly`, `PyInit_ugly` via `pyinst.extension_module`, and Java's `JNI_OnLoad` in `libugly_jni.so` (.NET's is `Ugly.dll` itself, see `dotnet/`). They link **no interpreter library**, because Debian's `python3` and `lua5.4` are statically linked and a second interpreter in the process would be fatal.

- On import, a binding calls `ugly_lang_register` (no dlopen) and `ugly_rt_attach` on the host interpreter. The Lua binding does it on the first use of `ugly` instead (`binding_attach`), on that thread, attaching the state's main thread: HAProxy loads states on its main thread and runs them on workers. Teardown is a `__gc` sentinel in Lua and `atexit` in Python.
- Every runtime, embedded or attached, preloads the same `ugly` module: `ugly.module(lang, name)` and `ugly.eval(lang, code)`, which use `ugly_rt_get`, plus `is_error`/`is_proxy` in Lua and `Error`/`Proxy`/`rethrow` in Python.
- The Lua plugin keeps its runtime in the registry (`ugly.rt`), not `LUA_EXTRASPACE`, and converts on the `lua_State` it was given, so it works inside coroutines.
