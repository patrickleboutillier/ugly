# Proposal: call options (keyword arguments, overload hints, call context)

Status: **for review** (2026-11-25). Once its open questions are decided, it
moves into [contract.md](contract.md) and the headers, the way the v2
contract did.

## The problem

A call carries positional arguments and nothing else. Three things that
languages put on a call have no way through:

1. **Keyword arguments.** Much of Python's standard library and PyPI
   takes them: `json.dumps(data, indent=2)`, `open(path, encoding="utf-8")`,
   `requests.get(url, timeout=5)`, `sorted(xs, key=f)`. From Lua, Perl, JS,
   Java or C they can't be passed today, and a Python proxy refuses them
   (`f(x, key=1)` raises `TypeError`).
2. **Overload hints**, v1's `hint`. Java picks an overload by scoring how
   well the arguments convert; a hint settles the ties (`f(int)` against
   `f(long)`, `f(Object)` against `f(String)`).
3. **Call context.** Perl calls run in list context and collapse (decision
   9). Some Perl code returns something else in scalar context
   (`localtime`, `reverse`, a count).

They are one problem: something about the call, not an argument. So: one
mechanism, call options.

## Proposal

### Core API (`ugly.h`)

```c
typedef struct ugly_kwarg {
	const char *name ;           /* UTF-8, NUL-terminated */
	ugly_val value ;             /* borrowed for the call, like positional arguments */
} ugly_kwarg ;

typedef struct ugly_call_opts {
	uint32_t size ;              /* sizeof(ugly_call_opts): fields can be added later */
	const ugly_kwarg *kwargs ;
	size_t nkwargs ;
	const char *hint ;           /* language-specific; NULL for none */
	const char *context ;        /* "list", "scalar" or "void"; NULL for the language's default */
} ugly_call_opts ;

#define UGLY_CALL_OPTS_INIT { sizeof(ugly_call_opts), NULL, 0, NULL, NULL }

ugly_status ugly_call_opts(ugly_ctx *ctx, ugly_handle fn, const ugly_val *args, size_t nargs,
	const ugly_call_opts *opts, ugly_kind want, ugly_val *out) ;
ugly_status ugly_invoke_opts(ugly_ctx *ctx, ugly_handle obj, const char *method, const ugly_val *args,
	size_t nargs, const ugly_call_opts *opts, ugly_kind want, ugly_val *out) ;
ugly_status ugly_new_opts(ugly_ctx *ctx, ugly_handle cls, const ugly_val *args, size_t nargs,
	const ugly_call_opts *opts, ugly_handle *out) ;
```

- `ugly_call`, `ugly_invoke` and `ugly_new` stay, as the forms with no
  options. With `opts` NULL or empty, the `_opts` forms are those.
- Keyword values are borrowed like positional ones: handles among them get
  borrow slots, revoked when the call returns.
- `size` lets fields be added (a timeout, say) without breaking callers.

### Plugin contract (`ugly_plugin.h`, ABI 4)

Three optional entries, `call_opts`, `invoke_opts` and `new_opts`, with the
same arguments as their plain forms plus `opts`. The core uses the plain
entry when there are no options. If there are options and the plugin has no
`_opts` entry, the call fails with `UGLY_ERR_NOT_SUPPORTED`, so options are
never silently dropped.

**Exact or refused**, as everywhere: a plugin either honours an option with
its language's own meaning, or refuses the call with
`UGLY_ERR_NOT_SUPPORTED`, naming the option. It never ignores one.

### What each language does

| | keyword arguments | hint | context |
|---|---|---|---|
| Python | `f(*args, **kwargs)`; a name the function doesn't take is Python's own `TypeError` (`RAISED`) | refused | refused |
| Perl | appended after the positional arguments as `name => value` pairs, in the order given (Perl's named-argument idiom, so `Point->new(x => 3, y => 4)`) | refused | `scalar`, `list` (the default, with decision 9's collapse), `void` |
| Java | refused (Java has none) | the parameter types of the overload to use, e.g. `"int,java.lang.String"`; no such overload is `UGLY_ERR_NOT_FOUND` | refused |
| Lua | refused (see question 2) | refused | refused |
| JS | refused (see question 2) | refused | refused |
| mock | implemented, for the conformance suite | refused | refused |

### In the bindings

Each binding needs a way to say "these are keyword arguments" that can't be
mistaken for a positional argument. The proposal is a marker made by the
binding's API, passed last:

| Binding | Keyword arguments | Other options |
|---|---|---|
| Python | native: `f(x, indent=2)` on a proxy | `ugly.call(f, *args, _hint=..., _context=..., **kw)` |
| Lua | `json.dumps(data, ugly.kw{ indent = 2 })` | `ugly.call(f, ugly.opts{ hint = "...", context = "scalar" }, ...)` |
| JS | `json.dumps(data, ugly.kw({ indent: 2 }))` | `ugly.call(f, ugly.opts({...}), ...)` |
| Perl | `json::dumps($data, Ugly::kw(indent => 2))` | `Ugly::call($f, Ugly::opts(context => 'scalar'), ...)` |
| Java | `json.invoke("dumps", data, Ugly.kw("indent", 2))` (and `Ugly.kw(Map)`) | `Ugly.opts().hint("int,long")` passed the same way |
| C | `ugly_call_opts` | the same struct |

Going the other way, a call that arrives with keyword arguments is shaped
by the callee's plugin (the table above), so a Perl sub called from Python
with `f(x, a=1)` gets `($x, a => 1)`.

### Conformance

A fixture `kw` in every language's conformance module, and a test group
`options`:

- keyword arguments reach the callee by name, mixed with positional ones,
  for call, invoke and new: exact for Python, Perl and mock, refused for the
  others (the "exact or refused" rule);
- a keyword the callee doesn't take: the callee's own error;
- borrowed keyword values are revoked after the call;
- `hint` picks the overload in Java, and is refused elsewhere;
- `context` gives Perl's scalar result, and is refused elsewhere;
- pairwise: keyword arguments between every pair that supports them.

## Open questions

1. **Names.** `ugly_call_opts` and friends, `ugly.kw`, `Ugly::kw`,
   `Ugly.kw`. Shorter or more explicit ones are welcome.
2. **Lua and JS: refuse keyword arguments, or map them to a trailing
   options table/object?** Lua and JS code often takes an options table
   last (`f(x, { indent = 2 })`), but that's a convention, not the
   language. Mapping would let a Python caller write `f(x, indent=2)` to a
   Lua function that takes an options table, and surprise one that
   doesn't. **Recommendation: refuse**, and let callers pass the table as
   an ordinary positional argument, which they can already do.
3. **Perl: keyword arguments as trailing pairs?** It's Perl's own idiom, and
   `Class->new(name => value)` is how most Perl constructors take
   arguments. The alternative is a trailing hash reference, used by some
   APIs. **Recommendation: pairs.**
4. **Java hint format.** Parameter type names joined by commas, matching
   `Class.getName()` (`int`, `java.lang.String`, `[I`). The alternative is
   a JNI-style signature (`(ILjava/lang/String;)`), which is exact but hard
   to write. **Recommendation: type names.**
5. **Should `hint` and `context` wait?** Keyword arguments are what users
   meet first. **Recommendation:** put all three fields in the struct now, so
   the ABI changes once, and implement keyword arguments first, then
   context, then hints, each with its tests.

## Order of work

1. The core form and plugin ABI 4, with the mock implementing keyword
   arguments; the `options` conformance group.
2. Python and Perl keyword arguments; the other plugins refuse.
3. The bindings' syntax (Python proxies, `ugly.kw` in Lua and JS,
   `Ugly::kw` in Perl, `Ugly.kw` in Java).
4. Perl's `context`; Java's `hint`.
5. README: `json.dumps(data, indent=2)` from Lua, Perl and Java.
