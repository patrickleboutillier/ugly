# The mock plugin

Notes for Claude Code, loaded when working in this directory. Shared rules: `plugins/CLAUDE.md` and the root `CLAUDE.md`.

**`mock/`** is a small language in C (lenient, Perl-like conversions) that implements every plugin rule and every fixture.

- Its keyed collection is the table (`M_TABLE`, `make_table()`): ENTRIES, with keys equal when they are the same kind and value (objects: the same object), and one method, `count`, which an entry may be named too (`table_method()`).
