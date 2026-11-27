# History and state

Where the project came from and what has been settled along the way. Open work is in [TODO.md](../TODO.md); the spec is [contract.md](contract.md).

## State (2026-11-27)

- **`master`**: the v2 core (libugly 2.8.0 on master, plugin ABI 6; 2.7.0 is the last release); plugins for the mock, Lua (5.4 or 5.3), CPython, JS (QuickJS), Perl 5, Java and C# (.NET), and Haskell (GHC); bindings for Lua (both versions), Python, JS (qjs), Perl, Java, .NET and Haskell (the package `ugly`, `Ugly.withUgly`). Tests: the conformance suite (88 runs: 11 groups × 8 plugins), pairwise (56 ordered pairs), `fixed-size` (the core alone, with a test language it registers), `symbols` (a host exporting the names of the core's internals), `rt-get` (threads asking together for an any-thread language's first runtime), the Haskell plugin's own 5, the binding tests (9, plus `configure-java`), `dotnet-signals`, `python-fork`, the examples (4, plus 4 HAProxy runs when `haproxy` is installed). 173 tests pass here; the 88 without a JVM, a CLR, GHC or HAProxy are clean under valgrind, the 28 with GHC (but no JVM or CLR) under `valgrindghc`, and helgrind finds no races. CI runs all of it.
- **Releases** (source tarballs only, `.github/workflows/release.yml`): v2.2.0 (the first), v2.3.0, v2.4.0, v2.5.0 (the C# plugin and binding, the language guides), v2.5.1 (the benchmarks and what they fixed: a lock-free handle lookup, cached reflection), v2.5.2 (Java's JNI calls cut, forking after Python started), v2.6.0 (Haskell: the plugin, its binding and music-diatonic; `UGLY_TRAIT_FIXED_SIZE`), v2.7.0 (iteration and `ugly_to_string` in the core and all eight languages; libugly 2.7.0, plugin ABI 5). `perf` is installed (`/usr/lib/linux-tools/*/perf`, user-space sampling works at paranoid 2). To release: set the version in `meson.build` (and `src/meson.build`'s library version if the API changed), merge, tag `vX.Y.Z` on master, push the tag; CI checks the tag against the version, runs `meson dist` (build and test from the tarball) and publishes. `gh workflow run release.yml` is a dry run. Releasing is outward-facing: only when the author asks.

- **Done, beyond the original plan:** Java and its binding; Perl's namespace import and ties; `ugly_configure`; threads (any-thread runtimes, runtimes the host serializes); Lua 5.3; foreign functions as Lua functions; call options (keyword arguments, Perl's context, Java's hints); CI; source releases; the HAProxy proof of concept, in both of HAProxy's Lua modes on several threads, and with Python started before HAProxy forks (fork handlers in the core and the Python plugin); the C# plugin (the CLR through `hostfxr`) and its binding; Haskell (GHC: compiled export tables, module libraries, the conformance suite, music-diatonic, the binding).
- **Decision 6** (Latin-1 decoding) was **revised** so that bytes round-trip; it's flagged to the author in the contract.
- **What Haskell made explicit** (2026-11-26, at the author's request): the contract and the headers' comments now state four assumptions Haskell broke without needing a core change: `want` may choose what runs (§1); a module may need building, and `eval`/`globals` may be absent (§2); a sequence may be infinite, so `length` may not return (§2); identity is best effort where the language has none (§3, plugin rule 6). Writing these down found a hang: `ugly_splice` called `length` for its range check before the plugin could refuse, so splicing `[0..]` never returned. Fixed (libugly 2.6.0, released in v2.6.0; 2026-11-27): `UGLY_TRAIT_FIXED_SIZE` (bit 4), which the core checks with `describe` before the range check (`fixed_size` in `protocol.c`); a plugin must report it for a sequence that may be unbounded (Haskell does for every immutable one, `elemsSplice` = `Nothing`), and may for others (the rest don't yet; their `splice` still refuses).

- **Tags:** `v0.01` is the original 2013–2014 code, including Java and JNA. `v0.02` is the v1 reference: the original contract with a Perl 5 plugin and XS binding, a Meson build and a TAP suite. v1 was removed from the tree on 2026-11-24 at the author's request.

- **The contract** was approved on 2026-11-24 (PR #1).
- **Haskell** is done (2026-11-26); the author decided against a cap on `length` of lazy lists (as in Haskell).

## Landscape (2026)

| Approach | What covers it today |
|---|---|
| In-process polyglot runtime | GraalVM Polyglot/Truffle (requires their VM and their language implementations) |
| Language-neutral interfaces + ABI | Wasm Component Model / WIT, WASI 0.2+ (requires compiling to Wasm, which shuts out native CPython/Perl/JVM ecosystems) |
| Plugin hosts | Extism, Wasmtime embeddings |
| Java ↔ native | Panama FFM API (final since Java 22) replaces JNI and JNA |
| Pairwise bridges | Inline::*, PyO3, JPype, napi-rs |
| Out-of-process | gRPC, Cap'n Proto, JSON-RPC |

**UGLY's niche** is embedding the *real* interpreters in one process and letting them call each other through a neutral C core. That niche is still largely unfilled.

## Docs

- `doc/design-essay.md` is in the author's first-person voice; the author verified its Inline::Java claims on 2026-11-24. It describes the 2013 design historically.
