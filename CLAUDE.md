# CLAUDE.md

## Project: UGLY (Ultimate Glue LaYer)

A C hub for in-process, multi-language interop: it embeds the real interpreters and routes every cross-language call through a neutral C core. It was first written in 2013–2014 by the author of Perl's **Inline::Java**, and revived in 2026 around a new contract, `doc/contract.md`.

Treat the author as a domain expert on language interop. Discuss design tradeoffs; don't explain JNI, callbacks or cross-runtime lifetimes.

## Where things are

- `doc/contract.md` is the spec. Its "Decisions" and "Clarifications" sections record everything settled. Every rule in it should be a test in `tests/`.
- `include/ugly.h` is the public API, and `include/ugly_plugin.h` the plugin contract (the plugin rules plus the core services).
- `src/` is the core (libugly 2.7.0, plugin ABI 5). Notes: `src/CLAUDE.md`.
- `plugins/<lang>/` holds the plugins (mock, lua, python, js, perl5, java, dotnet, haskell), each with its binding in `binding/`. Notes: `plugins/CLAUDE.md` and `plugins/<lang>/CLAUDE.md`. Read a plugin's notes before changing it.
- `tests/` is the conformance suite (10 groups × 8 plugins), pairwise, the binding tests and the benchmarks; `examples/` are programs run as tests. Notes: `tests/CLAUDE.md`.
- `doc/languages/<lang>.md` are the user guides, `doc/plugin-guide.md` is for plugin authors, `doc/benchmarks.md` has the measurements, and `doc/design-essay.md` is in the author's first-person voice. `README.md` is the only README.
- `TODO.md` has the open work, the 2026-11-27 review's findings, and what's done. `doc/history.md` has the tags, the releases and how to make one, decisions revised, and the 2026 landscape.

## Rules that are easy to get wrong

- Arguments are borrowed and revoked on return, so a plugin must `ugly_dup` what it keeps. Results are owned.
- `ugly_release`, `ugly_val_release` and `ugly_error_payload` don't clear the ctx's error; every call that returns a status does.
- A plugin reports a non-exception failure with `ugly_fail` (chosen status), an exception with `ugly_raise_native` (`RAISED` plus a payload), and a rethrown foreign value with `ugly_raise_handle`.
- A language's error must never `longjmp` across a core frame or skip one: catch it at the boundary.
- Signals belong to whoever installed them first: a plugin restores any handler its interpreter resets while starting (perl ignores SIGFPE, which the JVM needs).
- The core requires the plugin ABI to match exactly, so `Binding.cs`'s `PluginTable` moves with `ugly_plugin.h`.
- libugly exports only `ugly_*` (`src/ugly.map`).
- More of these, for the core and for each plugin, are in the notes beside the code.

## Build and test

- Meson (≥ 0.61) and Ninja, out-of-tree, with `-Wall` and no warnings: `meson compile -C build`, `meson test -C build`.
- `meson test -C build --setup valgrind` leaves out suites `jvm`, `clr`, `ghc` and `haproxy`; GHC tests use `--setup valgrindghc --suite ghc --no-suite jvm --no-suite clr`; `--setup helgrind` is for `threads-*`.

## Keeping the docs current

- Record any change to the contract in `doc/contract.md`, and in the headers.
- Keep `doc/languages/<lang>.md` current with any change to a plugin or binding; their examples were run when written.
- Add to `doc/plugin-guide.md` when a plugin teaches something new, and update `doc/benchmarks.md` when performance changes.
- Update the notes beside the code you changed (`src/CLAUDE.md`, `plugins/<lang>/CLAUDE.md`, `tests/CLAUDE.md`), `TODO.md`, and the state in `doc/history.md`.

## Process

- Work on a branch, open a PR, wait for CI, rebase-merge it, then delete the local branch. A PR touching only documentation (`*.md`, `doc/`, `LICENSE`, `.gitignore`) gets no CI run (`paths-ignore`), so merge it without waiting. GitHub deletes the remote branch on merge.
- Check that the merge happened (`gh pr view N --json state`) before relying on it: a failed merge once closed a PR when its branch was deleted by hand.
- The author lets Claude push and merge at will. Releasing is outward-facing: only when the author asks (the steps are in `doc/history.md`).
- Next: whatever the author picks from `TODO.md` (open items first, done ones at the end). Keep it current.
