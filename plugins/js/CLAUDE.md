# The JS plugin (QuickJS)

Notes for Claude Code, loaded when working in this directory. Shared rules: `plugins/CLAUDE.md` and the root `CLAUDE.md`.

**`js/`** is QuickJS, the language `"js"`, from the pinned wrap `subprojects/quickjs.wrap` (sha256-checked; `subprojects/packagefiles/quickjs/meson.build` builds the engine sources as a static PIC library, with `-w`).

- One JSRuntime plus JSContext per ugly runtime. Refs are boxed JSValues; identity is the object pointer.
- Proxies are two exotic classes (`proxy_class`, and `proxy_fn_class` with `call`), cached per owned handle in a C hash table that the finalizer clears.
- Numbers: integral within ±2^53 → INT, else DOUBLE; larger INTs go in as `BigInt`.
- Errors are `error_class` objects inheriting `Error.prototype`.
- Modules are ES modules from `UGLY_JS_PATH`, plus the built-in C module `"ugly"` (`module`, `eval`, `isError`, `isProxy`, `gc`).
- Jobs run when the outermost call returns (`enter`/`leave`), but not when attached, since the host owns the event loop. `JS_FreeRuntime` asserts that nothing is still referenced.
- Runtimes are found through a registry (`runtimes`), not the context opaque, because an attached context belongs to `qjs`. Refs are `jsbox` entries linked per runtime.
- The binding (`binding/`, `js_init_module`, loaded by `qjs` as `import "…/ugly.so"`) tears down from a global sentinel object, whose `gc_mark` declares every box to QuickJS's GC. Otherwise the external references would keep `qjs`'s context alive past its `JS_FreeRuntime` assertion. The subproject also builds `qjs` (`qjsc`, `repl.c`, `-rdynamic`) for the binding test.
- Editing `subprojects/packagefiles/quickjs/` needs `rm -rf subprojects/quickjs-2026-06-04` (git-ignored) to take effect: Meson copies `packagefiles/` only at extraction.
