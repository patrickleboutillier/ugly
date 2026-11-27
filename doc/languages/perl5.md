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
| `Ugly::iter($x)` | an iterator over a foreign iterable, a Perl array, or an iterator (itself): `next` returns the next value, or the empty list at the end |
| `Ugly::is_proxy($v)`, `Ugly::is_error($e)` | whether a value is foreign, or an error from another language |

The binding attaches to the running perl at `use`, and tears down in an
`END` block.

## Foreign values in Perl

A foreign value is an `Ugly::Proxy` object, one per value (`==`, `eq`, `!=`
and `ne` compare identity). Every method called on it is the foreign
value's, whatever its name; only `can`, `isa`, `DOES` and `VERSION` are
Perl's own.

- `$p->{k}`, `keys %$p`, `exists`, `delete`: members, as a tied hash.
- `$p->[0]`, `@$p`, `push`, `pop`, `shift`, `unshift`, `splice`: elements,
  0-based, as a tied array (resizing goes through the protocol's `splice`).
- `$p->m(@args)` invokes `m` with the foreign language's receiver rules;
  `$p->(@args)` calls it.
- `"$p"` is the foreign language's string for it (Python's `str()`, Java's
  `toString()`...), or `Ugly::Proxy(<handle>)` if it has none.
- `Ugly::iter($p)` iterates it (see [Iteration](#iteration)).
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
- Arrays are iterable, and so are objects with a `next` method, which are
  iterators (see [Iteration](#iteration)).
- Stringification is `"$x"`, overloads included; a package's is its name.

## Iteration

Perl has no iteration protocol, so UGLY takes its most common convention:
an **iterator is an object with a `next` method**, which returns the next
value, or the empty list at the end (and again after). Returning the empty
list rather than `undef` lets `undef` be a value. `Ugly::iter` gives such
an object for anything iterable, so a loop is the same over every
language's values:

```perl
use Ugly python => 'itertools' ;

my $it = Ugly::iter(itertools::count(10)) ;      # Python's infinite counter
while (my ($n) = $it->next){
    last if $n > 12 ;
    print "$n\n" ;                               # 10, 11, 12
}
my $squares = Ugly::eval("python", "(x * x for x in range(4))") ;
my $sq = Ugly::iter($squares) ;
my @all ;
while (my ($x) = $sq->next){ push @all, $x }      # 0, 1, 4, 9: one step per next
```

A foreign value its language doesn't iterate but that has elements (a Lua
table) is walked by index; a Perl array gives an `Ugly::Iterator` over its
indices; an iterator is returned as it is.

Other languages iterate Perl arrays and Perl iterators. A closure makes a
generator:

```perl
package Countdown ;
sub new { my ($class, $n) = @_ ; bless { n => $n }, $class }
sub next { my $s = shift ; return if $s->{n} <= 0 ; $s->{n}-- }

package main ;
Ugly::eval("python", "sum")->(Countdown->new(3)) ;   # 6: Python's sum() loops over it
```

Each `next` runs when the other language asks for a value, never ahead.
When the last reference goes, the iterator is freed (its `DESTROY` runs),
which is how an infinite one ends.

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
`message` field, if it's a hash), or `die` and the string. An error perl
itself raises while another language uses a Perl value (a store into a
restricted hash, a tied `FETCH` that dies) is that operation's error, in
the same way. An exception object whose `""` overload dies has its class's
name for a message.

## Threads

Each Perl interpreter belongs to the thread that created it: calls from
another thread fail with `UGLY_ERR_WRONG_THREAD`. A process can have several
(`UGLY_CAP_MULTI_RUNTIME`).

With ithreads, a new thread gets no copy of the proxies and errors its
parent holds (`CLONE_SKIP`): they are undefined there. Foreign values stay
with the thread that got them.

## Configuring

Key: `path` (`@INC`), ahead of `PERL5LIB`:
`Ugly::configure(perl5 => { path => './lib' })` from another Perl, or
`ugly.configure("perl5", path=["./lib"])` from Python.

## Pitfalls

- **Hash keys are text.** A member name from another language finds the
  character key, or else the key that is its UTF-8 bytes (text read from a
  file and never decoded); a new key is stored as characters. Keys are
  listed as text the same way, a Latin-1 key's characters included.
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
- **Proxies stringify as their language does.** `"$p"` asks the foreign
  language (`Ugly::Proxy(…)` if it can't), and `$p + 1` does arithmetic
  on that string, not on the foreign value. Fetch the value first.
- **An iterator's `next` must return the empty list at the end**, not
  `undef` (a value): `return ;`, not `return undef`. And `can` must find
  `next` without `AUTOLOAD`. `ref($obj)` is `Ugly::Proxy`, even for an instance of a
  package `use Ugly` made (TODO).
- **`AUTOLOAD`.** `use Ugly` defines subs on first use through `AUTOLOAD`;
  a closure that `AUTOLOAD` compiles sees `$Ugly::AUTOLOAD`, not the
  target package's.
- **Package names.** `os.path` and `java.util.List` map to `os::path` and
  `java::util::List`; a name that doesn't make a package name needs one
  given (`use Ugly lang => { 'name' => 'Pkg' }`).
- **SIGFPE.** perl ignores SIGFPE when it starts; the plugin puts back
  whatever was there (the JVM needs it).
