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

console.log("1.." + count);
if (failures > 0) throw new Error(failures + " test(s) failed");
