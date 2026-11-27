// The JS binding: a plain qjs program uses Python and Lua through ugly.so (TAP).
// UGLY_JS_BINDING is the path of the built ugly.so.

import * as std from "std";

let count = 0, failures = 0;
function ok(cond, what) {
  count++;
  if (!cond) failures++;
  console.log((cond ? "ok " : "not ok ") + count + " - " + what);
}

const ugly = await import(std.getenv("UGLY_JS_BINDING"));
ok(typeof ugly.module === "function" && typeof ugly.eval === "function", "import ugly.so in a plain qjs program");

// Options for Lua, before anything starts it.
ugly.configure("lua", { path: [std.getenv("UGLY_TEST_CONFIGURED") + "/?.lua"] });

const py = ugly.module("python", "conformance");
ok(ugly.isProxy(py), "ugly.module('python', ...) gives a Python module, as a proxy");
ok(py.echo("héllo") === "héllo", "a string goes to Python and back");
ok(py.apply(x => x * 2, 21) === 42, "Python calls a JS arrow function");

const c = py.make_counter();
c.inc();
ok(c.inc() === 2, "a method call on a Python object: c.inc()");
ok(c.count === 2, "a member of a Python object: c.count");
ok(c.items.length === 2 && c.items[0] === 1 && c.items[1] === 2, "a Python list: length and 0-based indexing");
ok(py.echo(c) === c, "a Python object round trip gives the same proxy (===)");
const obj = {};
ok(py.echo(obj) === obj, "a JS object round trip gives the original");

try {
  py.raise_error("boom");
  ok(false, "a Python error is thrown");
} catch (e) {
  ok(ugly.isError(e) && e instanceof Error && e.type === "ConformanceError" && e.message === "boom" && e.language === "python",
     "a Python error is an Error, with type, message and language");
}

const mine = new Error("mine");
try {
  py.apply(() => { throw mine; }, 1);
  ok(false, "a JS error crossing Python");
} catch (e) {
  ok(e === mine, "a JS error crossing Python comes back as the same object");
}

const math = ugly.module("python", "math");
ok(Math.abs(math.sqrt(2) - 1.4142135623731) < 1e-9, "Python's math.sqrt from JS");
ok(ugly.module("lua", "string").upper("abc") === "ABC", "Lua's string.upper from JS");

// README's example: Python's standard library only.
const functools = ugly.module("python", "functools");
ok(functools.reduce((a, b) => a * b, [1, 2, 3, 4, 5]) === 120,
   "functools.reduce calls a JS arrow function over a JS array");
const date = ugly.module("python", "datetime").date(2026, 11, 25);
ok(date.isoformat() === "2026-11-25" && date.year === 2026, "a Python datetime.date: isoformat() and year");
ok(ugly.eval("python", "6 * 7") === 42, "ugly.eval('python', code)");

// Python code using JS through its own ugly module, reaching back into this very interpreter.
globalThis.shared = { answer: 42 };
ok(ugly.eval("python", "__import__('ugly').eval('js', 'shared.answer')") === 42,
   "embedded Python uses ugly.eval('js', ...) to reach this qjs program's globals");

// Python keeps a JS function past the end of the script: qjs must still exit cleanly
// (its JS_FreeRuntime asserts that nothing is referenced).
py.kept = () => 42;
ok(py.kept() === 42, "Python keeps a JS function (until exit)");

ok(ugly.module("lua", "ugly_configured").where === "configured", "ugly.configure('lua', { path: ... }): Lua finds the module");
let late = false;
try { ugly.configure("lua", { path: "/too/late" }); } catch (e) { late = ugly.isError(e); }
ok(late, "  ... and configuring it again once started throws");

// Keyword arguments (contract §11): ugly.kw({...}) last.
ok(py.kw(1, ugly.kw({ c: 3, b: 2 })) === "1 2 3", "keyword arguments reach Python: kw(1, ugly.kw({ c: 3, b: 2 }))");
ok(new py.KwBox(ugly.kw({ value: 5 })).add(ugly.kw({ n: 10 })) === 15, "  ... new py.KwBox(ugly.kw({ value: 5 })).add(...)");
ok(ugly.module("python", "builtins").int("ff", ugly.kw({ base: 16 })) === 255, "int('ff', base=16)");

let refused = false;
try { py.kw(1, ugly.opts({ context: "scalar" })); } catch (e) { refused = ugly.isError(e) && /no call context/.test(e.message); }
ok(refused, "ugly.opts({ context: ... }) into Python: refused");
ok(py.kw(1, ugly.kw({ c: 3 }), ugly.opts({})) === "1 0 3", "  ... and ugly.kw and ugly.opts last, in either order");

// Iteration (contract §2): for...of, spread and String() on foreign values.
const itertools = ugly.module("python", "itertools");
const firsts = [];
for (const x of itertools.count(1)) {
  if (x > 3) break;
  firsts.push(x);
}
ok(firsts.join(",") === "1,2,3", "for...of over an infinite Python iterator, left with break");
ok([...py.count_to(3)].join(",") === "0,1,2", "spread of a Python generator: [...py.count_to(3)]");
const words = [...ugly.module("lua", "string").gmatch("one two", "%a+")];
ok(words.join(",") === "one,two", "spread of a Lua iterator function: string.gmatch");
const squares = ugly.eval("lua", "local t = {} for i = 1, 3 do t[i] = i * i end return t");
ok([...squares].join(",") === "1,4,9", "a Lua table, which Lua doesn't iterate: its elements, as an array-like");
let threw = false;
try { for (const x of py.fail_after(1)) {} } catch (e) { threw = ugly.isError(e) && e.type === "ConformanceError"; }
ok(threw, "a Python generator's error ends the loop");
ok(py.sum_iter([1, 2, 3, 4]) === 10, "Python's for over a JS array");
ok(py.sum_iter((function* () { yield 5; yield 6; })()) === 11, "Python's for over a JS generator");
const c2 = py.make_counter();
c2.inc();
ok(String(c2) === "counter(1)" && `${c2}` === "counter(1)", "String(c) and a template: Python's str(c)");

// A symbol names no foreign member: storing under one is refused, not stored under its description.
const symDict = ugly.eval("python", "{}");
let symRefused = false;
try { symDict[Symbol("tag")] = 1; } catch (e) { symRefused = e instanceof TypeError; }
symDict.plain = 2;
ok(symRefused && ugly.eval("python", "lambda d: ','.join(sorted(d))")(symDict) === "plain",
	"proxy[Symbol()] = v is a TypeError, and stores nothing");

// A foreign sequence as an array: push, pop, shift, unshift and splice, over ugly_splice.
const fseq = ugly.eval("python", "[10, 20, 30]");
ok(fseq.push(40, 50) === 5 && fseq.unshift(5) === 6 && [...fseq].join(",") === "5,10,20,30,40,50",
	"push and unshift on a Python list give the new length: " + [...fseq].join(","));
ok(fseq.pop() === 50 && fseq.shift() === 5 && [...fseq].join(",") === "10,20,30,40", "pop and shift: " + [...fseq].join(","));
const cut = fseq.splice(1, 2, "a", "b", "c");
ok(Array.isArray(cut) && cut.join(",") === "20,30" && [...fseq].join(",") === "10,a,b,c,40",
	"splice(1, 2, ...) gives what it removed, as an array: " + [...fseq].join(","));
ok(fseq.splice(-1).join(",") === "40" && fseq.splice(0).length === 4 && fseq.length === 0, "splice(-1) and splice(0)");
// A Python list has a pop of its own, which comes first; a Lua table has none, so it gets an array's.
const ltab = ugly.eval("lua", "return { 7 }");
ok(ltab.pop() === 7 && ltab.pop() === undefined && ltab.shift() === undefined && ltab.length === 0,
	"pop and shift on a Lua table, and undefined once it's empty");
ok(ugly.eval("python", "{'a': 1}").push === undefined, "a foreign value that isn't a sequence has no push");

// delete p.k removes the foreign member; an element isn't deleted (a foreign sequence has no holes).
const fdict = ugly.eval("lua", "return { a = 1, b = 2 }");
const deleted = delete fdict.a;
ok(deleted && fdict.a === undefined && fdict.b === 2, "delete proxy.a removes the member");
let kept = false;
try { delete ugly.eval("python", "[1, 2]")[0]; } catch (e) { kept = e instanceof TypeError; }
ok(kept, "delete proxy[0] is refused (a TypeError in a module)");

console.log("1.." + count);
if (failures > 0) throw new Error(failures + " test(s) failed");
