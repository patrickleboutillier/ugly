import java.util.ArrayList;
import java.util.List;
import java.util.function.BinaryOperator;
import java.util.function.Function;

import ugly.Foreign;
import ugly.ForeignException;
import ugly.Ugly;

/**
 * The Java binding: a plain java program uses Lua, Python, JavaScript and
 * Perl through ugly.Ugly (TAP). The last test is printed by a Lua finalizer
 * when UGLY tears down at JVM exit.
 */
public class test_binding {
	static final int PLAN = 39;
	static int count = 0;

	static void ok(boolean cond, String what){
		count++;
		System.out.println((cond ? "ok " : "not ok ") + count + " - " + what);
	}

	interface Body { void run() throws Throwable; }

	/** Passes a new object to Lua and keeps only a weak reference to it. */
	static java.lang.ref.WeakReference<Object> passOne(Foreign lua){
		Object o = new Object();
		lua.invoke("echo", o);
		return new java.lang.ref.WeakReference<>(o);
	}

	/** Entries in ugly.Foreign's cache (package-private: for tests). */
	static int cached() throws Exception {
		java.lang.reflect.Method m = Foreign.class.getDeclaredMethod("cached");
		m.setAccessible(true);
		return (Integer)m.invoke(null);
	}

	/** Runs a test body; an unexpected exception fails it. */
	static void check(String what, Body body){
		try {
			body.run();
		}
		catch (Throwable t){
			ok(false, what + ": " + t);
		}
	}

	public static void main(String[] args){
		System.out.println("1.." + PLAN);
		Ugly.configure("python", "path", System.getenv("UGLY_TEST_CONFIGURED"));     // before Python starts

		Foreign lua = (Foreign)Ugly.module("lua", "conformance");
		ok(lua != null, "Ugly.module(\"lua\", ...) in a plain java program gives a Foreign");

		check("strings", () -> {
			String s = "héllo 😀";
			ok(s.equals(lua.invoke("echo", s)), "a string with a non-BMP character goes to Lua and back");
		});

		check("callbacks", () -> {
			Function<Object, Object> twice = x -> (Long)x * 2;
			ok(Long.valueOf(42).equals(lua.invoke("apply", twice, 21)), "Lua calls a Java lambda");
		});

		check("objects", () -> {
			Foreign c = (Foreign)lua.invoke("make_counter");
			c.invoke("inc");
			ok(Long.valueOf(2).equals(c.invoke("inc")), "a method call on a Lua object");
			ok(Long.valueOf(2).equals(c.get("count")), "a member of a Lua object");
			ok(((Foreign)c.get("items")).length() == 2, "a Lua list's length");
			ok(lua.invoke("echo", c) == c, "a Lua object round trip gives the same Foreign");
			Object mine = new Object();
			ok(lua.invoke("echo", mine) == mine, "a Java object round trip gives the original");
		});

		check("errors", () -> {
			try {
				lua.invoke("raise_error", "boom");
				ok(false, "a Lua error throws ForeignException");
			}
			catch (ForeignException e){
				ok("ConformanceError".equals(e.type()) && "boom".equals(e.getMessage()) && "lua".equals(e.language()),
					"a Lua error throws ForeignException, with type, message and language");
			}
			RuntimeException mine = new IllegalStateException("mine");
			Function<Object, Object> thrower = x -> { throw mine; };
			try {
				lua.invoke("apply", thrower, 1);
				ok(false, "a Java exception crossing Lua");
			}
			catch (IllegalStateException e){
				ok(e == mine, "a Java exception crossing Lua comes back as the same object");
			}
		});

		check("lua eval", () -> {
			ok(Long.valueOf(42).equals(Ugly.eval("lua", "return 6 * 7")), "Ugly.eval(\"lua\", code)");
			Foreign add = (Foreign)Ugly.eval("lua", "return function(a, b) return a + b end");
			ok(Long.valueOf(5).equals(add.call(2, 3)), "a Lua function called from Java");
		});

		check("interfaces", () -> {
			Foreign f = (Foreign)Ugly.eval("lua",
				"return function(list) list:replaceAll(function(x) return x * 10 end) return list end");
			List<Object> list = new ArrayList<>(List.of(1L, 2L, 3L));
			Object back = f.call(list);
			ok(back == list && list.equals(List.of(10L, 20L, 30L)),
				"a Lua function passed for a Java interface (List.replaceAll's UnaryOperator)");
		});

		check("python", () -> {
			Foreign math = (Foreign)Ugly.module("python", "math");
			ok(Double.valueOf(4.0).equals(math.invoke("sqrt", 16.0)), "Python's math.sqrt");
			try {
				Ugly.eval("python", "1 // 0");
				ok(false, "a Python error throws ForeignException");
			}
			catch (ForeignException e){
				ok("ZeroDivisionError".equals(e.type()) && "python".equals(e.language()),
					"a Python error throws ForeignException (ZeroDivisionError)");
			}
			Foreign json = (Foreign)Ugly.module("python", "json");
			ok("[1, 2, 3]".equals(json.invoke("dumps", Ugly.eval("python", "[1, 2, 3]"))), "Python's json.dumps");
		});

		check("readme", () -> {
			// README's example: standard libraries only.
			Foreign functools = (Foreign)Ugly.module("python", "functools");
			BinaryOperator<Object> times = (a, b) -> (Long)a * (Long)b;
			ok(Long.valueOf(120).equals(functools.invoke("reduce", times, List.of(1L, 2L, 3L, 4L, 5L))),
				"functools.reduce calls a Java lambda over a Java list");
			Foreign string = (Foreign)Ugly.module("lua", "string");
			ok("3 items at 9.50".equals(string.invoke("format", "%d items at %.2f", 3, 9.5)),
				"Lua's string.format");
			Foreign date = (Foreign)((Foreign)Ugly.module("python", "datetime")).invoke("date", 2026, 11, 25);
			ok("2026-11-25".equals(date.invoke("isoformat")) && Long.valueOf(2026).equals(date.get("year")),
				"a Python datetime.date: isoformat() and year");
		});

		check("configure", () -> {
			Foreign m = (Foreign)Ugly.module("python", "ugly_configured");
			ok("configured".equals(m.get("where")), "Ugly.configure(\"python\", \"path\", ...): Python finds the module");
		});

		check("keyword arguments", () -> {
			Foreign py = (Foreign)Ugly.module("python", "conformance");
			ok("1 2 3".equals(py.invoke("kw", 1, Ugly.kw("c", 3, "b", 2))), "keyword arguments reach Python: Ugly.kw(\"c\", 3, \"b\", 2)");
			java.util.Map<String, Object> m = new java.util.LinkedHashMap<>();
			m.put("base", 16);
			ok(Long.valueOf(255).equals(((Foreign)Ugly.module("python", "builtins")).invoke("int", "ff", Ugly.kw(m))), "  ... and from a Map: int(\"ff\", base=16)");
			try {
				((Foreign)Ugly.module("lua", "string")).invoke("upper", "x", Ugly.kw("a", 1));
				ok(false, "keyword arguments to Lua are refused");
			}
			catch (ForeignException e){
				ok(e.getMessage().contains("takes no call options"), "keyword arguments to Lua are refused");
			}
		});

		check("iteration", () -> {
			Foreign itertools = (Foreign)Ugly.module("python", "itertools");
			List<Long> firsts = new java.util.ArrayList<>();
			for (Object x : (Foreign)itertools.invoke("count", 1)){
				if ((Long)x > 3) break;
				firsts.add((Long)x);
			}
			ok(firsts.equals(List.of(1L, 2L, 3L)), "for-each over an infinite Python iterator, left with break");
			Foreign words = (Foreign)((Foreign)Ugly.module("lua", "string")).invoke("gmatch", "one two", "%a+");
			List<Object> ws = new java.util.ArrayList<>();
			words.forEach(ws::add);
			ok(ws.equals(List.of("one", "two")), "forEach over a Lua iterator function (string.gmatch)");
			Foreign squares = (Foreign)Ugly.eval("lua", "local t = {} for i = 1, 3 do t[i] = i * i end return t");
			List<Object> sq = new java.util.ArrayList<>();
			for (Object x : squares) sq.add(x);
			ok(sq.equals(List.of(1L, 4L, 9L)), "a Lua table, which Lua doesn't iterate: walked by index");
			Foreign py = (Foreign)Ugly.module("python", "conformance");
			ok(Long.valueOf(10).equals(py.invoke("sum_iter", List.of(1L, 2L, 3L, 4L))), "Python's for over a Java List");
			ok(Long.valueOf(3).equals(py.invoke("sum_iter", java.util.stream.Stream.of(1L, 2L))), "  ... and over a Java Stream");
			ok("{'a': 1}".equals(Ugly.eval("python", "{'a': 1}").toString()), "toString() of a Foreign: Python's str()");
		});

		check("js", () -> {
			Foreign a = (Foreign)Ugly.eval("js", "[1, 2, 3].map(x => x * 2)");
			ok(a.length() == 3 && Long.valueOf(4).equals(a.get(1)), "a JavaScript array: length and elements");
		});

		check("perl", () -> {
			Foreign join = (Foreign)Ugly.eval("perl5", "sub { join '-', @_ }");
			ok("a-b-c".equals(join.call("a", "b", "c")), "a Perl sub called from Java");
			Foreign want = (Foreign)Ugly.eval("perl5", "sub { wantarray ? 'list' : 'scalar' }");
			ok("scalar".equals(want.call(Ugly.opts().context("scalar"))), "Ugly.opts().context(\"scalar\") last: Perl's scalar context");
		});

		check("threads", () -> {
			Object[] got = new Object[3];
			Thread t = new Thread(() -> {
				try { got[0] = ((Foreign)Ugly.module("python", "math")).invoke("sqrt", 9.0); }
				catch (Throwable e){ got[0] = e; }
				try { got[1] = ((Foreign)Ugly.module("lua", "string")).invoke("upper", "abc"); }
				catch (Throwable e){ got[1] = e; }
				try { got[2] = lua.invoke("echo", 1); }
				catch (Throwable e){ got[2] = e; }
			});
			t.start();
			t.join();
			ok(Double.valueOf(3.0).equals(got[0]), "another Java thread gets its own ctx, and shares Python (UGLY_CAP_ANY_THREAD): " + got[0]);
			ok("ABC".equals(got[1]), "  ... and gets its own Lua: " + got[1]);
			ok(got[2] instanceof RuntimeException && String.valueOf(((Throwable)got[2]).getMessage()).contains("another thread"),
				"  ... but the main thread's Lua values stay the main thread's: " + got[2]);
		});

		check("leaks", () -> {
			// A program that only calls out: what it passed is released at its next use of UGLY.
			java.lang.ref.WeakReference<Object> w = passOne(lua);
			for (int i = 0 ; (i < 100)&&(w.get() != null) ; i++){
				Ugly.eval("lua", "collectgarbage() collectgarbage()");     // Lua drops its proxy of it
				lua.invoke("echo", 1);
				System.gc();
				Thread.sleep(10);
			}
			ok(w.get() == null, "a Java object passed to Lua is collectable after the next use of UGLY");

			// A collected Foreign leaves the cache.
			for (int i = 0 ; i < 2000 ; i++){
				Ugly.eval("lua", "return {}");
			}
			for (int i = 0 ; (i < 100)&&(cached() > 500) ; i++){
				System.gc();
				Thread.sleep(10);
			}
			ok(cached() <= 500, "collected Foreigns leave the cache: " + cached() + " entries after 2000 values");
		});

		check("teardown", () -> {
			Ugly.eval("lua", "_G.at_exit = setmetatable({}, { __gc = function() "
				+ "io.write('ok " + PLAN + " - UGLY tears down at JVM exit (a Lua finalizer ran)\\n') io.stdout:flush() end })");
			ok(count == PLAN - 2, "every test before teardown ran");
		});
		System.out.flush();
	}
}
