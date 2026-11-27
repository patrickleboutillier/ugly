# The Perl 5 plugin

Notes for Claude Code, loaded when working in this directory. Shared rules: `plugins/CLAUDE.md` and the root `CLAUDE.md`.

**`perl5/`** is Perl 5 (5.34 here, MULTIPLICITY), written in C against the perl API with no XS generator.

- `ENTER_PERL`/`LEAVE_PERL` set and restore `PERL_GET_CONTEXT` on every entry, and are a scope for temporaries (`ENTER; SAVETMPS` ... `FREETMPS; LEAVE`): a caller that isn't Perl has none around the entry.
- Every entry runs under `eval` (`protect`, through the XS sub `Ugly::_guard` called with `G_EVAL`; the vtable's `g_*` functions): perl's API croaks (a restricted hash, a tied `FETCH`, an overloaded `""`), and outside any eval that ends the process, or jumps over the core's frames to the host's eval. Calls skip the guard unless their target is tied, and `call_perl` converts a result under it only when that can run code (`runs_code`): the guard doubled a call's cost. `DIED` is the test for `$@` (a ref is an error whatever its `bool` says); `message_of` stringifies an exception under eval, and reads a byte string that isn't UTF-8 as Latin-1 (through a variable: `SvPVutf8_nolen` evaluates its argument twice).
- An XS function re-reads perl's stack pointer (`SPAGAIN`) after a call through the core, which can come back into perl and move the stack (`xs_proxy_keys`). `Ugly::Proxy` and `Ugly::Error` have `CLONE_SKIP`: an ithread's copy would release the parent's handle.
- The XS functions are in `Ugly::Core` (`Ugly::Core::_get`...), not `Ugly::Proxy`: every method called on a proxy must reach `AUTOLOAD`. Hash member names are UTF-8 keys (`key_len`: a negative length), or the same bytes as a byte key when only that exists (`member_len`); keys are listed as UTF-8.
- Entries on a proxy (contract §2): `Ugly::Proxy::Hash` (the tie behind `%$proxy`) uses `Ugly::Core::_entry_*` when the value has ENTRIES, members otherwise; a key that reads as an integer is tried as one too. `Ugly::entry` and friends take any key. (A Perl hash as ENTRIES is still to do.) `message_of` copies a string exception by hand: `sv_mortalcopy` of a temporary steals its buffer, and emptied the payload.
- Refs are SVs. Foreign values are `Ugly::Proxy`: a blessed scalar ref holding the handle, cached per handle. Its Perl side (overloads, `AUTOLOAD`, complete tied hash and array, and `Ugly::import`) is `helper.pm`, turned into the C string `helper_pm` at build time by `embed.pl` and defined in every interpreter; the XS functions are registered with `newXS`.
- `use Ugly python => 'json'` binds a module to a package (`bind_module`): callable members become subs on first use (`AUTOLOAD`), classes nested packages with `new`. An `AUTOLOAD` closure sees `$Ugly::AUTOLOAD`, the package it was compiled in, not the target's.
- Modules and classes are `Ugly::Package` objects (a scalar ref holding the package name). Their identity is the stash, members are subs and nested packages, and `new` instantiates.
- Calls run in list context and collapse (`call_perl`). Never `PUSHMARK` before `eval_sv`: it pushes its own mark.
- The binding (`binding/`: `lib/Ugly.pm`, `auto/Ugly/Ugly.so`, `boot_Ugly`) attaches to the running perl. `END { Ugly::_teardown() }` tears down. Its test sets `PERL_DESTRUCT_LEVEL=2`, so the host frees everything and valgrind measures only UGLY.

## Embedding perl: what v1 taught

- Every entry into an embedded perl must save and restore `PERL_GET_CONTEXT`.
- Call `PERL_SYS_INIT3` once per process, only if `PL_curinterp` is NULL. **Never** call `PERL_SYS_TERM`: after it, perl can't start again in the same process (`perl_alloc` exits with EBADF).
- Delete a runtime with `perl_destruct`/`perl_free` under its context.
- `call_sv` mortalizes its arguments. Whatever wraps an RV must own exactly one reference (v1's `wrap` took two, so objects were never DESTROYed).
