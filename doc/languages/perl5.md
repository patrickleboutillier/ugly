# Perl 5

Perl 5 built with MULTIPLICITY (5.34 and 5.38 are tested), language
`"perl5"`. How Perl maps onto the protocol is the Perl 5 column of
[the contract's §10](../contract.md#10-the-languages); this page is the
practical side.

## From Perl: the binding

The binding is the module `Ugly` (`plugins/perl5/binding/`: `lib/Ugly.pm`
and `auto/Ugly/Ugly.so`). Put both directories on `PERL5LIB` if it isn't
installed.

`use Ugly LANG => MODULES` binds modules of other languages to Perl
packages: their functions become subs, their classes nested packages with
`new`:

```perl
use Ugly python => [ 'math', 'json' ], java => 'java.util.ArrayList' ;

math::sqrt(16) ;                                  # 4
my $l = json::loads('[1, 2, 3]') ;                # a Python list, as an array ref
push @$l, 4 ;                                     # Python sees [1, 2, 3, 4]
my $al = java::util::ArrayList->new ;             # classes are packages with new()
use Ugly java => { 'java.util.HashMap' => 'HashMap' } ;   # or name the package yourself
```

| Function | What it does |
|---|---|
| `Ugly::module($lang, $name)` | a module of `$lang`, as a proxy (no package) |
| `Ugly::eval($lang, $code)` | evaluates code in a language that can (`UGLY_CAP_EVAL`) |
| `Ugly::configure($lang => { key => value or [ ... ] }, ...)` | options for a language that hasn't started: in a `BEGIN` block, before `use Ugly LANG => ...` starts it |
| `Ugly::kw(name => value, ...)` | keyword arguments, passed last |
| `Ugly::opts(hint => ..., context => ...)` | other call options, passed last (beside a `kw`, in either order) |
| `Ugly::is_proxy($v)`, `Ugly::is_error($e)` | whether a value is foreign, or an error from another language |

The binding attaches to the running perl at `use`, and tears down in an
`END` block.

## Foreign values in Perl

A foreign value is an `Ugly::Proxy` object, one per value (`==` and `eq`
compare identity).

- `$p->{k}`, `keys %$p`, `exists`, `delete`: members, as a tied hash.
- `$p->[0]`, `@$p`, `push`, `pop`, `shift`, `unshift`, `splice`: elements,
  0-based, as a tied array (resizing goes through the protocol's `splice`).
- `$p->m(@args)` invokes `m` with the foreign language's receiver rules;
  `$p->(@args)` calls it.
- A class bound by `use Ugly` is a package with `new`. A class proxy you
  hold (from `Ugly::module`, or a member) constructs when called, if it can
  only be instantiated (a Java or Perl class): `$cls->(@args)`. `$cls->new`
  would invoke a method named `new`.

## Perl values elsewhere

| Perl | goes out as | comes back as |
|---|---|---|
| `undef` | NULL | `undef` |
| a boolean (`!!1`) | BOOL | `!!1` / `!!0` |
| IOK / NOK scalar | INT / DOUBLE | IV / NV |
| string with the UTF-8 flag | STRING | a flagged string |
| string without it | STRING if valid UTF-8, else BYTES | BYTES: a byte string |
| reference (hash, array, code, object) | a handle | the original reference |

- A hash's members are its keys; an object's are its keys, then its
  methods. An array has elements. A code ref is callable.
- Modules and classes are packages (`Ugly::Package` objects): their members
  are subs and nested packages, and `new` instantiates.
- `invoke` passes the invocant for an object, and none for a package's
  function.

## Perl as a guest

`ugly_rt_import` does `require Module` and gives the package.
`ugly_rt_globals` is `main::`, and `eval` evaluates a string of Perl.
Calls into Perl run in **list context** and collapse: nothing gives null,
one value gives that value, several give an array.

## Calls and options

Perl honours two call options: keyword arguments, appended as
`name => value` pairs in the order given (so `kw(1, c => 3)` sees
`@_ = (1, 'c', 3)`), and a context, `list` (the default), `scalar` or
`void`:

```python
localtime = ugly.eval("perl5", "sub { localtime($_[0]) }")
localtime(0, ugly.opts(context="scalar"))         # a date string, not a 9-element list
```

Hints are refused.

## Errors

An error from another language is an `Ugly::Error` object with `type`,
`message`, `language` and `payload` methods, which stringifies as
`[language] type: message`. Catch it with `eval {}`; one that escapes Perl
goes on unchanged, and `die $e->payload` rethrows the original exception.

A Perl `die` going out has the object's class as its type (and its
`message` field, if it's a hash), or `die` and the string.

## Threads

Each Perl interpreter belongs to the thread that created it: calls from
another thread fail with `UGLY_ERR_WRONG_THREAD`. A process can have several
(`UGLY_CAP_MULTI_RUNTIME`).

## Configuring

Key: `path` (`@INC`), ahead of `PERL5LIB`:
`Ugly::configure(perl5 => { path => './lib' })` from another Perl, or
`ugly.configure("perl5", path=["./lib"])` from Python.

## Pitfalls

- **List context.** A sub that returns a list gives other languages an
  array; one that returns one value gives that value. `wantarray` is true
  unless the caller asked for another context.
- **Strings are bytes until decoded.** An unflagged string that isn't valid
  UTF-8 goes out as BYTES (decision 6 in the contract). Text read from a
  file without a decoding layer is unflagged UTF-8, which goes out right.
  Asking for `want = STRING` decodes an unflagged non-UTF-8 string as
  Latin-1.
- **Booleans before 5.36** are recognised by `PL_sv_yes`/`PL_sv_no`'s
  signature, a heuristic; 5.36 and later have `SvIsBOOL`.
- **Proxies stringify as a debug form.** `"$p"` is `Ugly::Proxy(…)`, and
  `$p + 1` does arithmetic on that string, not on the foreign value. Fetch
  the value first. `ref($obj)` is `Ugly::Proxy`, even for an instance of a
  package `use Ugly` made (TODO).
- **`AUTOLOAD`.** `use Ugly` defines subs on first use through `AUTOLOAD`;
  a closure that `AUTOLOAD` compiles sees `$Ugly::AUTOLOAD`, not the
  target package's.
- **Package names.** `os.path` and `java.util.List` map to `os::path` and
  `java::util::List`; a name that doesn't make a package name needs one
  given (`use Ugly lang => { 'name' => 'Pkg' }`).
- **SIGFPE.** perl ignores SIGFPE when it starts; the plugin puts back
  whatever was there (the JVM needs it).
