# Proposal: iterators (and "to string")

*Draft for review, 2026-11-27. Once the open questions are settled, this
moves into [contract.md](contract.md) (§2, §9, §10, Decisions) and the
headers, and this file goes away, as the call options proposal did.*

## Why

The protocol can walk a foreign sequence only by index: `get_index` until
`UGLY_ERR_NOT_FOUND`. That leaves three gaps.

- **Lazy sequences cost quadratic time.** A Haskell list is a linked list,
  so `get_index(i)` walks i cells. Python's `for x in proxy` falls back to
  `__getitem__` until `IndexError`, so it takes O(n²) over a Haskell list,
  and so does any binding that walks by index.
- **Generators can't cross at all.** A Python generator, a JS generator, a
  Java `Iterator`, a C# `yield return` method, `string.gmatch` in Lua: none
  has elements, so a caller in another language can't consume one. It can
  only call methods on it by hand (`__next__`, `next()`, `MoveNext`), which
  is the receiver-specific code the protocol exists to avoid.
- **Native loops don't work over proxies.** `for x of proxy` in JS, `for
  (Object x : foreign)` in Java and `foreach` in C# have nothing to hook.

`FIXED_SIZE` was the last fix of this kind: it refuses splicing an infinite
list. Iteration is the positive half, a way to consume one.

## The proposal

### The trait and two operations

A new trait, `UGLY_TRAIT_ITERABLE` (bit 5): the value's language can
iterate it. **Exact or refused**: a value is iterable when its own
language's `for` loop accepts it, and it yields what that loop yields.
Nothing is synthesized.

```c
#define UGLY_TRAIT_ITERABLE  (1u << 5)

/* An iterator over h, owned by the caller; its home is h's. UGLY_ERR_NOT_CAPABLE if h isn't iterable. */
ugly_status ugly_iter(ugly_ctx *ctx, ugly_handle h, ugly_handle *out) ;

/*
 * Runs iterator 'it' for at most 'max' more values, converted to 'want',
 * into out[0..*got). *got < max means it is exhausted; *got == 0 means it
 * already was. On an error, the *got values before it are valid and owned,
 * and the error is the ctx's.
 */
ugly_status ugly_next(ugly_ctx *ctx, ugly_handle it, ugly_kind want, ugly_val *out, size_t max, size_t *got) ;
```

The plugin entries (ABI 5) are the same shape:

```c
ugly_status (*iter)(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out) ;
ugly_status (*next)(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_kind want, ugly_val *out, size_t max, size_t *got) ;
```

Rules:
- **An iterator is an ordinary handle.** Its home is the iterable's
  runtime, so the thread rules, depth limit, borrows and errors apply
  unchanged. An iterator is iterable too, and `ugly_iter` on one returns
  it (Python's `iter(it) is it`). A plugin does this for Java's
  `Iterator` and C#'s `IEnumerator` as well, since those languages have no
  such rule.
- **Laziness is kept.** `next` runs at most `max` steps and never reads
  ahead: a generator's body runs only as far as the caller asked, and an
  infinite one is fine. Batching (`max > 1`) is for callers that will
  consume that many anyway, such as Python's `list(p)` or a conversion to
  a native array. A `for` loop asks for one at a time.
- **Release closes.** No `ugly_close`. Dropping the last reference to an
  iterator closes it as its language would when a loop ends early: a
  Python generator's `close()` (its `finally` runs), JS's `return()`
  (what `break` in `for…of` calls), C#'s `Dispose()` (what `foreach`
  does). For a consumer that holds the iterator in a proxy, "when" is
  that proxy's finalizer, as for any handle.
- **Errors while iterating are the language's own.** Mutating a dict
  during iteration gives Python's `RuntimeError`, as `RAISED`. A generator
  that raises is `RAISED`, with the payload.
- **After exhaustion**, further `next` calls give whatever the language
  gives, normally `*got == 0` again. The core doesn't remember.

### What each language iterates

| | iterable | each step yields | release does |
|---|---|---|---|
| Python | anything `iter()` accepts (`__iter__`, or the sequence protocol) | what `for` gives (a dict's keys) | drops the iterator; a generator closes |
| JS | objects with `Symbol.iterator`: arrays, `Map`, `Set`, generators, strings never (they cross as STRING) | `value`, until `done`; a `Map` gives `[k, v]` arrays | `return()`, if not done |
| Lua | functions, as `for v in f do` (Q3); a coroutine | the first value returned, until `nil` | nothing |
| Perl 5 | arrays (Q4) | elements | nothing |
| Java | `Iterable`, arrays, `Iterator`, `BaseStream` (streams) | elements; a `Map` isn't iterable, as in Java | nothing; a stream is closed |
| C# | `IEnumerable` (arrays, collections, `yield return`, LINQ), `IEnumerator` | elements; a dictionary gives `KeyValuePair` handles | `Dispose()` |
| Haskell | lists (lazy: infinite ones too); boxes that declare it | elements, forced as `get_index` forces them | nothing |
| mock | lists, and a generator fixture | elements | counts it, for the tests |
| C (host) | core lists | elements | nothing |

### Consumers: proxies iterate natively

Each plugin's proxies take part in their language's loop when the foreign
value is `ITERABLE`. This is plugin work, so embedded runtimes get it as
well as bindings.

| | how | otherwise |
|---|---|---|
| Python | `__iter__` → an iterator proxy whose `__next__` calls `ugly_next` | today's index walk, for `ELEMENTS` without `ITERABLE` |
| JS | `[Symbol.iterator]` on the proxy | |
| Lua | `for v in ugly.iter(p) do` (Q7) | |
| Perl 5 | `my $it = Ugly::iter($p); while (my ($x) = $it->next) {...}` and `each` (Q7) | the tied array's index walk |
| Java | `ugly.Foreign implements Iterable<Object>`: `for (Object x : f)` | |
| C# | `Foreign : IEnumerable<object?>`: `foreach`, LINQ | |
| Haskell | `FromUgly [a]` takes an iterable as a lazy list, read as it's forced (Q7) | today's eager conversion, for `ELEMENTS` |

## "To string", in the same ABI bump

A smaller gap from TODO.md: a proxy can't stringify with its own
language's rule. Perl's `"$p"` gives a debug form, and Python's
`str(proxy)` gives the proxy's `repr`.

```c
/* h as its language makes it a string: str(), tostring, "", String(), toString(), ToString(), show. */
ugly_status ugly_to_string(ugly_ctx *ctx, ugly_handle h, ugly_val *out) ;
```

This is not `want = STRING`, which converts only what the language
converts implicitly (Python refuses an object). `to_string` is the explicit
stringification, the one `print` uses. Haskell has it only for a type with
`Show` (a box's text), and refuses otherwise. Proxies use it for `str()`,
`tostring`, `"$p"`, `String(p)`, `toString()` and `ToString()`. Each
language's debug form (`repr`, Perl's `Ugly::Proxy=SCALAR(...)`) stays
what it is.

## Open questions

Each has a recommendation.

1. **How the end is signalled.** Recommended: a count, `*got < max`. It
   costs nothing. The alternatives are `UGLY_ERR_NOT_FOUND`, which records
   an error at the end of every loop, and a new status, which would be the
   first non-error status besides `UGLY_OK`.
2. **Batching in the ABI from the start.** Recommended: yes, `max` in both
   the public function and the plugin entry, so the ABI changes once.
   A crossing costs 60 to 870 ns from C (doc/benchmarks.md: Lua least,
   Java and Haskell most), so `list(p)` over a million elements is where
   it shows. A plugin may
   simply loop its one-step code.
3. **Lua functions are iterable.** Lua has no iterator objects. Its
   iterators are functions, and the generic `for` calls one until `nil`
   (`gmatch`, `io.lines`, `coroutine.wrap`). Recommended: yes, every Lua
   function reports `ITERABLE` with exactly that meaning, so Python can
   `for m in lua_gmatch_result`. The cost is that `describe` says
   "iterable" of every Lua function, including ones that never return
   `nil`. A caller only iterates when its own code says so (a `for`), so
   that is the language's meaning, not a hazard the core adds. Tables
   aren't iterable: Lua's `for` doesn't take one without `pairs`, and
   their members and elements already cross.
4. **Perl: arrays only.** Perl has no iteration protocol. `for (@a)` is an
   array, a hash has three readings (`keys`, `values`, `each`), and
   iterator objects are convention, not language. Recommended: arrays
   only; refuse hashes, code refs and objects.
5. **One value per step.** Recommended: yes. Languages that iterate pairs
   yield them as their own values (JS `[k, v]`, C# `KeyValuePair`). Lua's
   extra return values are dropped, as `for v in f` drops them. A
   key-value form can come with the hash protocol if it's needed.
6. **No fallback in the core.** Recommended: the core doesn't make an
   `ELEMENTS` value iterable by walking its indexes. That would be the
   quadratic walk again, hidden. Bindings keep their own index walk where
   they have one (Python's `__getitem__`, Perl's tied array).
7. **Binding syntax.** These are idiom choices, yours more than mine:
   - Lua: `ugly.iter(p)`, giving a function for the generic `for`. `pairs`
     isn't used: 5.4 still honours `__pairs`, but it means key-value.
   - Perl: `Ugly::iter($p)`, an object whose `next` returns the empty list
     at the end, so `undef` elements survive. `each @$p` would come from
     the same object.
   - Haskell: a lazy list (`unsafeInterleaveIO`, like `hGetContents`). An
     error raised while it's forced becomes a `ForeignError` thrown from
     pure code, as lazy IO does. `Ugly.next :: Foreign -> IO (Maybe
     Value)` is there for code that wants control.
8. **`to_string` rides along.** Recommended: yes, in ABI 5, since it's one
   entry per plugin and the ABI changes once.

## Tests

A tenth conformance group, `iterators`, "exact or refused" as usual. The
fixtures are written in each language's own idiom:

- `count_to(n)` lazily yields 0 to n−1 (a Python generator, a JS generator
  function, a Lua closure, a Java `Iterator`, a C# `yield return`, Haskell's
  `[0..n-1]`, an array in Perl). `produced()` reports how many values the
  last one has produced. Taking 3 of `count_to(1000)` gives
  `produced() == 3` (SKIP in Perl, which has nothing lazy).
- `naturals()` is infinite. Taking 5 terminates.
- `fail_after(n)` yields n values, then raises `ConformanceError`. The
  checks: n values, then `RAISED` with the payload, and the batch form
  delivers the n before the error.
- `sum_iter(xs)` and `take_iter(xs, n)` consume a foreign iterable with the
  language's own loop, the second breaking early. This tests the proxies;
  `take_iter` also checks that the iterator was released (`finalized()`,
  or a generator's `finally` in the fixture).
- Batches: `max` larger than what remains; `max` of 1; `next` after
  exhaustion.

Pairwise: A's `count_to` summed by B's `sum_iter`, B's `take_iter` over
A's `naturals()`, and A's `fail_after` raised through B's loop and caught
in C with the original payload.

`to_string`: `make_counter()`'s type defines its language's stringification
(`__str__`, `__tostring`, overloaded `""`, `toString`, `ToString`, a `Show`
instance) as `counter(<count>)`, checked exactly.

## Order of work

1. **Contract and core.** The decisions go into contract.md and the
   headers. Then the core (`ugly_iter`, `ugly_next`, `ugly_to_string`;
   libugly 2.7.0, plugin ABI 5), the mock, core lists, and the `iterators`
   group, with every other plugin refusing.
2. **Plugins, one PR each or a few together**: producers and consumers.
   Python, Haskell (the case that motivated it), JS, Lua, C#, Java, Perl.
3. **Bindings' syntax** (Q7), Perl's `each`, the language guides, contract
   §10's columns, the plugin guide.
