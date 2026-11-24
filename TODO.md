# TODO

Ideas and open work, roughly in priority order within each section. The
agreed plan lives in [doc/contract.md](doc/contract.md) ("Order of work");
this list is everything else, plus the reasons behind each item.

## Now

- [x] **QuickJS plugin** (done 2026-11-24: `plugins/js/`).
- [ ] **Perl**, ported from tag `v0.02` (see "Languages").

## Embedding in real hosts: HAProxy and friends

The motivating case: an application that embeds Lua as its extension
language (HAProxy, nginx/OpenResty, Neovim, Wireshark, game engines), with
the logic you need already written in Python. `ugly.so` links no Lua of its
own, so it already uses the host's. These items stand between that and a
working setup.

- [ ] **Calling a runtime from any thread** (the real blocker). HAProxy runs
  Lua from several worker threads: one shared state behind a lock
  (`lua-load`), or one state per thread (`lua-load-per-thread`). UGLY binds
  a runtime to the thread that created it, and Python has one runtime per
  process, so every other thread gets `UGLY_ERR_WRONG_THREAD`.
  - Proposal: a capability, `UGLY_CAP_ANY_THREAD`, for plugins that
    serialize access themselves. Python does already, since every entry
    takes the GIL, which works from any thread.
  - The core already has one ctx per thread and a locked handle table. The
    work is in the runtime thread check in `rt_enter`, deferred releases,
    and the threads conformance group.
  - The contract anticipated this relaxation (§6).
- [ ] **Lazy Python initialization**, for hosts that fork. In HAProxy's
  master-worker mode, a `lua-load` script may run in the master before the
  workers fork, and each worker would inherit a forked interpreter (that
  would need `PyOS_AfterFork_Child`). Initializing on first use, inside the
  worker, avoids it.
- [ ] **Lua 5.3 support** (HAProxy builds commonly use 5.3). The plugin uses
  5.4-only API such as `lua_newuserdatauv`. A thin compatibility layer can
  bridge it, and the binding would be built once per Lua version found.
- [ ] **Lua 5.1 / LuaJIT support**: OpenResty, Neovim, many games. It's a
  bigger API gap (no integers, `lua_pcall` and registry differences,
  `luaL_setfuncs`), but it reaches the most embedded Luas.
- [ ] **HAProxy proof of concept**, as a test when `haproxy` is installed: a
  small config whose Lua action calls into a Python module.
- [ ] **Document the blocking model.** A call into another language is a
  plain C call, so Lua can't yield across it. That's fine for quick work
  (lookups, scoring, validation, reusing parsing or policy code), but code
  that does I/O belongs behind the host's own asynchronous mechanisms, or
  out of process (HAProxy's SPOE). With several threads, the GIL serializes
  all the Python work.

## The contract: what v2.0 left out

All additive; none of it breaks existing plugins (contract, "Scope").

- [ ] **Keyword arguments** for foreign calls. Python's `f(x, key=...)` on a
  proxy is refused today, and so is `sorted(t, key=f)` passing through to
  another language. This needs a call-options form of `ugly_call`/`ugly_invoke`.
- [ ] **Iterators and generators**: a protocol for "next" (Python
  iterators, Lua `pairs`/closures, JS iterators), beyond today's indexing.
- [ ] **A hash protocol for non-string keys**: Lua tables keyed by tables,
  Python dicts keyed by tuples, JS `Map`.
- [ ] **Overload hints** (v1's `hint`), for statically typed languages, as
  part of the call options above. It's needed if Java comes back.
- [ ] **Async/coroutines across runtimes.** It's hard, and probably needs
  each plugin to expose its scheduler.
- [ ] **Python subinterpreters** (per-interpreter GIL, 3.12+), which would
  give Python `UGLY_CAP_MULTI_RUNTIME`.
- [ ] **Cross-runtime cycles** leak by design (contract §3). Revisit only
  with a real use case.
- [ ] **Sandboxing**: out of scope unless a host needs it.

## Languages

- [x] **QuickJS** (done).
- [ ] **Perl**, ported from tag `v0.02`. The lessons learned are in
  CLAUDE.md ("Notes for the Perl port"). It's worth it for codebases that
  are stuck in Perl and need to be called from elsewhere.
  - `/usr/bin/perl` is statically linked, so its binding registers the
    plugin, like Lua and Python.
  - Open questions: list vs scalar context (likely a call option beside
    `want`), dualvars, byte vs character strings, `DESTROY` timing.
- [ ] **A JS binding**, for programs that embed QuickJS themselves. There's
  no standard QuickJS host executable to attach to, the way there is for
  Lua and Python, so this waits for a real host.
- [ ] **Java** through Panama FFM, only if there's a reason. Creating the
  JVM still needs the JNI invocation API.

## Engineering

- [ ] **CI**: a GitHub Actions workflow that builds with and without each
  optional language, then runs `meson test`, the valgrind setup, and
  helgrind on the threads group. Today, every check runs only on the
  author's machine.
- [ ] **Attach tests in C.** Attach mode is tested only through the binding
  scripts. A C test that creates its own Lua state and attaches to it would
  bring attach into the conformance suite proper.
- [ ] **Install layout.** `meson install` warns that the Python extension's
  destination may not be on `sys.path` under `/usr/local`. Set
  `python.platlibdir` in the defaults, or document `-Dpython.install_env`.
- [ ] **Plugin helper directory.** `init(ctx, dir)` passes the plugin's
  directory, but no plugin uses it yet. Use it for language-side helper
  files, or drop it.
- [ ] **GitHub setting**: "Automatically delete head branches" is off, so
  merged branches are deleted by hand. That's the author's call.

## Docs

- [ ] **Design essay epilogue.** `doc/design-essay.md` ends with "what a v2
  would have to decide first". v2 now exists, so a short epilogue could say
  which of those decisions held, and what the implementation changed (the
  depth limit, receivers across languages, rule 7, static interpreters and
  bindings).
