# The JS plugin (QuickJS)

Notes for Claude Code, loaded when working in this directory. Shared rules: `plugins/CLAUDE.md` and the root `CLAUDE.md`.

**`js/`** is QuickJS, the language `"js"`, from the pinned wrap `subprojects/quickjs.wrap` (sha256-checked; `subprojects/packagefiles/quickjs/meson.build` builds the engine sources as a static PIC library, with `-w`).

- One JSRuntime plus JSContext per ugly runtime. Refs are boxed JSValues; identity is the object pointer.
- Proxies are two exotic classes (`proxy_class`, and `proxy_fn_class` with `call`), cached per owned handle in a C hash table that the finalizer clears.
- Numbers: integral within ±2^53 → INT, else DOUBLE; larger INTs go in as `BigInt`.
- Errors are `error_class` objects inheriting `Error.prototype`.
- Modules are ES modules from `UGLY_JS_PATH`, plus the built-in C module `"ugly"` (`module`, `eval`, `isError`, `isProxy`, `gc`).
- Jobs run when the outermost call returns (`enter`/`leave`), but not when attached, since the host owns the event loop. `JS_FreeRuntime` asserts that nothing is still referenced.
- A finalizer never releases on the spot while something on the thread will run the queue (`release_later`, `will_run`): releasing can run anything, a Perl `DESTROY` calling back into JS, or `js_release` calling an open iterator's `return()`, and no JS may run inside QuickJS's GC. The queue runs (`run_later`) at `need_ctx` (JS calling out), in `leave()`, at the end of `rt_delete` and of the binding's teardown; it keeps the ctx's error, as `leave()` does around the jobs it runs.
- `describe`: a class is INSTANTIABLE and not CALLABLE (`is_class`: `Function.prototype.toString` begins with "class"; cached in the box), a function both. So the bindings' "calling what can only be instantiated constructs it" covers JS classes.
- `call_or_construct`: a native constructor JS won't call (its `TypeError` "constructor requires 'new'", raised before it does anything) is constructed instead; describe can't tell it from one that may be called.
- A foreign sequence's `push`, `pop`, `shift`, `unshift` and `splice` are `seq_method` (over `ugly_splice`), given by `proxy_get_own_property` only when the value has no member of that name. `delete p.k` is `ugly_delete_member`; an element or a symbol isn't deleted.
- Entries on a proxy (contract §2): a property read is the member, then the entry (`p[3]`: the entry 3, else "3"); writes and `delete` go to entries when the value has ENTRIES; own keys are the entry keys a property can spell. `ugly.entry`, `setEntry`, `deleteEntry`, `entries` for any key. (JS's own `Map` as ENTRIES is still to do.)
- A symbol-keyed store on a proxy is a `TypeError` (`proxy_define_own_property`), as symbol reads come from the prototype.
- Runtimes are found through a registry (`runtimes`), not the context opaque, because an attached context belongs to `qjs`. Refs are `jsbox` entries linked per runtime.
- The binding (`binding/`, `js_init_module`, loaded by `qjs` as `import "…/ugly.so"`) tears down from a global sentinel object, whose `gc_mark` declares every box to QuickJS's GC. Otherwise the external references would keep `qjs`'s context alive past its `JS_FreeRuntime` assertion. The subproject also builds `qjs` (`qjsc`, `repl.c`, `-rdynamic`) for the binding test.
- Editing `subprojects/packagefiles/quickjs/` needs `rm -rf subprojects/quickjs-2026-06-04` (git-ignored) to take effect: Meson copies `packagefiles/` only at extraction.
