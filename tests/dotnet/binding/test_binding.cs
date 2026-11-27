// The .NET binding: a plain .NET program uses Lua, Python, JavaScript, Perl
// and Java through Ugly.Interop (TAP). Languages that weren't built
// (UGLY_TEST_LANGS) are SKIPped. The last test is printed by a Lua finalizer
// when Interop.Shutdown() tears UGLY down.
using Ugly;

static class TestBinding {
    const int Plan = 44;
    static int count;
    static readonly HashSet<string> langs = new((Environment.GetEnvironmentVariable("UGLY_TEST_LANGS") ?? "").Split(','));

    static void Ok(bool cond, string what) {
        count++;
        Console.WriteLine($"{(cond ? "ok" : "not ok")} {count} - {what}");
    }

    // Runs a test body of 'tests' tests; an unexpected exception fails it. Without a language it needs, they're SKIPped.
    static void Check(string what, int tests, string[] needs, Action body) {
        string? missing = needs.FirstOrDefault(l => !langs.Contains(l));
        if (missing != null) {
            for (int i = 0; i < tests; i++) Console.WriteLine($"ok {++count} - {what} # SKIP {missing} isn't built");
            return;
        }
        int before = count;
        try {
            body();
        }
        catch (Exception e) {
            Ok(false, $"{what}: {e.GetType().Name}: {e.Message}");
        }
        while (count < before + tests) Ok(false, $"{what}: a test didn't run");
    }

    // Passes a new object to Lua and keeps only a weak reference to it.
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static WeakReference PassOne(Foreign lua) {
        object o = new object();
        lua.Invoke("echo", o);
        return new WeakReference(o);
    }

    static int Main() {
        Console.WriteLine($"1..{Plan}");
        Interop.Configure("python", "path", Environment.GetEnvironmentVariable("UGLY_TEST_CONFIGURED")!);     // before Python starts

        Foreign lua = (Foreign)Interop.Module("lua", "conformance")!;
        Ok(Interop.IsForeign(lua), "Interop.Module(\"lua\", ...) in a plain .NET program gives a Foreign");

        Check("strings", 1, ["lua"], () => {
            string s = "héllo 😀";
            Ok(s.Equals(lua.Invoke("echo", s)), "a string with a non-BMP character goes to Lua and back");
        });

        Check("callbacks", 1, ["lua"], () => {
            Func<object?, object?> twice = x => (long)x! * 2;
            Ok(42L.Equals(lua.Invoke("apply", twice, 21)), "Lua calls a C# lambda");
        });

        Check("objects", 5, ["lua"], () => {
            var c = (Foreign)lua.Invoke("make_counter")!;
            c.Invoke("inc");
            Ok(2L.Equals(c.Invoke("inc")), "a method call on a Lua object");
            Ok(2L.Equals(c.Get("count")), "a member of a Lua object");
            Ok(((Foreign)c.Get("items")!).Length == 2, "a Lua list's length");
            Ok(ReferenceEquals(lua.Invoke("echo", c), c), "a Lua object round trip gives the same Foreign");
            var mine = new object();
            Ok(ReferenceEquals(lua.Invoke("echo", mine), mine), "a C# object round trip gives the original");
        });

        Check("errors", 2, ["lua"], () => {
            try {
                lua.Invoke("raise_error", "boom");
                Ok(false, "a Lua error throws ForeignException");
            }
            catch (ForeignException e) {
                Ok(e.ErrorType == "ConformanceError" && e.Message == "boom" && e.Language == "lua",
                    "a Lua error throws ForeignException, with type, message and language");
            }
            var mine = new InvalidOperationException("mine");
            Func<object?, object?> thrower = x => throw mine;
            try {
                lua.Invoke("apply", thrower, 1);
                Ok(false, "a C# exception crossing Lua");
            }
            catch (InvalidOperationException e) {
                Ok(ReferenceEquals(e, mine), "a C# exception crossing Lua comes back as the same object");
            }
        });

        Check("lua eval", 2, ["lua"], () => {
            Ok(42L.Equals(Interop.Eval("lua", "return 6 * 7")), "Interop.Eval(\"lua\", code)");
            var add = (Foreign)Interop.Eval("lua", "return function(a, b) return a + b end")!;
            Ok(5L.Equals(add.Call(2, 3)), "a Lua function called from C#");
        });

        Check("dynamic", 3, ["python"], () => {
            dynamic json = Interop.Module("python", "json")!;
            dynamic data = json.loads("{\"name\": \"ugly\", \"scores\": [3, 1, 4]}");
            Ok("ugly".Equals(data["name"]) && 3L.Equals(data["scores"][0]), "through dynamic: a Python dict and list");
            dynamic builtins = Interop.Module("python", "builtins")!;
            Ok(255L.Equals(builtins.@int("ff", @base: 16)), "named arguments are keyword arguments: int(\"ff\", base: 16)");
            Ok("[1, 2]".Equals(json.dumps(Interop.Eval("python", "[1, 2]"))), "json.dumps of a Python list, through dynamic");
        });

        Check("python", 2, ["python"], () => {
            var math = (Foreign)Interop.Module("python", "math")!;
            Ok(4.0.Equals(math.Invoke("sqrt", 16.0)), "Python's math.sqrt");
            try {
                Interop.Eval("python", "1 // 0");
                Ok(false, "a Python error throws ForeignException");
            }
            catch (ForeignException e) {
                Ok(e.ErrorType == "ZeroDivisionError" && e.Language == "python", "a Python error throws ForeignException (ZeroDivisionError)");
            }
        });

        Check("readme", 1, ["python"], () => {
            var functools = (Foreign)Interop.Module("python", "functools")!;
            Func<object?, object?, object?> times = (a, b) => (long)a! * (long)b!;
            Ok(120L.Equals(functools.Invoke("reduce", times, new List<object> { 1L, 2L, 3L, 4L, 5L })),
                "functools.reduce calls a C# lambda over a C# list");
        });

        Check("configure", 1, ["python"], () => {
            var m = (Foreign)Interop.Module("python", "ugly_configured")!;
            Ok("configured".Equals(m.Get("where")), "Interop.Configure(\"python\", \"path\", ...): Python finds the module");
        });

        Check("call options", 3, ["python", "lua"], () => {
            var py = (Foreign)Interop.Module("python", "conformance")!;
            Ok("1 2 3".Equals(py.Invoke("kw", 1, Interop.Kw("c", 3, "b", 2))), "Interop.Kw(\"c\", 3, \"b\", 2) last: keyword arguments reach Python");
            dynamic dpy = py;
            Ok("1 0 3".Equals(dpy.kw(1, c: 3)), "  ... and named arguments through dynamic");
            try {
                ((Foreign)Interop.Module("lua", "string")!).Invoke("upper", "x", Interop.Kw("a", 1));
                Ok(false, "keyword arguments to Lua are refused");
            }
            catch (ForeignException e) {
                Ok(e.Message.Contains("takes no call options"), "keyword arguments to Lua are refused");
            }
        });

        Check("iteration", 7, ["python", "lua"], () => {
            var itertools = (Foreign)Interop.Module("python", "itertools")!;
            var firsts = new List<long>();
            foreach (object? x in (Foreign)itertools.Invoke("count", 1)!) {
                if ((long)x! > 3) break;
                firsts.Add((long)x!);
            }
            Ok(string.Join(",", firsts) == "1,2,3", "foreach over an infinite Python iterator, left with break");
            var gen = (Foreign)Interop.Eval("python", "(x * x for x in range(10))")!;
            Ok(string.Join(",", gen.Take(3)) == "0,1,4", "LINQ over a Python generator: Take(3)");
            var words = (Foreign)((Foreign)Interop.Module("lua", "string")!).Invoke("gmatch", "one two", "%a+")!;
            Ok(string.Join(",", words) == "one,two", "foreach over a Lua iterator function (string.gmatch)");
            var squares = (Foreign)Interop.Eval("lua", "local t = {} for i = 1, 3 do t[i] = i * i end return t")!;
            Ok(string.Join(",", squares) == "1,4,9", "a Lua table, which Lua doesn't iterate: walked by index");
            var py = (Foreign)Interop.Module("python", "conformance")!;
            Ok(10L.Equals(py.Invoke("sum_iter", new List<long> { 1, 2, 3, 4 })), "Python's for over a C# List");
            Ok(3L.Equals(py.Invoke("sum_iter", Enumerable.Range(1, 2).Select(i => (long)i))), "  ... and over a LINQ query, run as Python asks");
            Ok("{'a': 1}" == ((Foreign)Interop.Eval("python", "{'a': 1}")!).ToString(), "ToString() of a Foreign: Python's str()");
        });

        Check("host re-entry", 1, ["python"], () => {
            Ok(5L.Equals(Interop.Eval("python", "__import__('ugly').module('dotnet', 'System.Math').Abs(-5)")),
                "Python reaches back into this program's CLR (ugly.module(\"dotnet\", ...))");
        });

        Check("js", 1, ["js"], () => {
            var a = (Foreign)Interop.Eval("js", "[1, 2, 3].map(x => x * 2)")!;
            Ok(a.Length == 3 && 4L.Equals(a[1]), "a JavaScript array: length and elements");
        });

        Check("perl", 2, ["perl5"], () => {
            var join = (Foreign)Interop.Eval("perl5", "sub { join '-', @_ }")!;
            Ok("a-b-c".Equals(join.Call("a", "b", "c")), "a Perl sub called from C#");
            var want = (Foreign)Interop.Eval("perl5", "sub { wantarray ? 'list' : 'scalar' }")!;
            Ok("scalar".Equals(want.Call(Interop.Opts(context: "scalar"))), "Interop.Opts(context: \"scalar\") last: Perl's scalar context");
        });

        Check("java", 2, ["java"], () => {
            var str = (Foreign)Interop.Module("java", "java.lang.String")!;
            Ok("65.0".Equals(str.Invoke("valueOf", 65, Interop.Opts(hint: "double"))), "Interop.Opts(hint: \"double\") last: Java's valueOf(double)");
            var list = (Foreign)((Foreign)Interop.Module("java", "java.util.ArrayList")!).Call()!;
            list.Invoke("add", "x");
            Ok(1L.Equals(list.Invoke("size")), "calling a Java class constructs it");
        });

        Check("threads", 3, ["python", "lua"], () => {
            object?[] got = new object?[3];
            var t = new Thread(() => {
                try { got[0] = ((Foreign)Interop.Module("python", "math")!).Invoke("sqrt", 9.0); } catch (Exception e) { got[0] = e; }
                try { got[1] = ((Foreign)Interop.Module("lua", "string")!).Invoke("upper", "abc"); } catch (Exception e) { got[1] = e; }
                try { got[2] = lua.Invoke("echo", 1); } catch (Exception e) { got[2] = e; }
            });
            t.Start();
            t.Join();
            Ok(3.0.Equals(got[0]), $"another C# thread shares Python (UGLY_CAP_ANY_THREAD): {got[0]}");
            Ok("ABC".Equals(got[1]), $"  ... and gets its own Lua: {got[1]}");
            Ok(got[2] is ForeignException e && e.Message.Contains("thread"), $"  ... but the main thread's Lua values stay the main thread's: {(got[2] as Exception)?.Message}");
        });

        Check("enumerables", 2, ["python"], () => {
            // An enumerable that is its own enumerator (yield, LINQ): each loop over it is a new pass.
            var twice = (Foreign)Interop.Eval("python", "lambda xs: str([list(xs), list(xs)])")!;
            Ok("[[0, 1, 2], [0, 1, 2]]".Equals(twice.Call(Enumerable.Range(0, 3))), $"Python iterates a LINQ enumerable twice: {twice.Call(Enumerable.Range(0, 3))}");
            var broken = (Foreign)Interop.Eval("python", "lambda xs: str([next(iter(xs)), list(xs)])")!;
            Ok("[0, [0, 1, 2]]".Equals(broken.Call(Enumerable.Range(0, 3))), "  ... and again after an iteration left unfinished");
        });

        Check("numbers", 1, ["python"], () => {
            // An integer for a method that takes double, decimal or float: the double one, not a tie.
            var round = (Foreign)Interop.Eval("python", "lambda: [__import__('ugly').module('dotnet', 'System.Math').Round(5), __import__('ugly').module('dotnet', 'System.MathF').Sqrt(4)] == [5.0, 2.0]")!;
            Ok(true.Equals(round.Call()), "Math.Round(5) from Python is Round(double), and MathF.Sqrt(4) still takes a float");
        });

        Check("leaks", 1, ["lua"], () => {
            // A program that only calls out: what it passed is released at its next use of UGLY.
            WeakReference w = PassOne(lua);
            for (int i = 0; i < 100 && w.IsAlive; i++) {
                Interop.Eval("lua", "collectgarbage() collectgarbage()");     // Lua drops its proxy of it
                lua.Invoke("echo", 1);
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }
            Ok(!w.IsAlive, "a C# object passed to Lua is collectable after the next use of UGLY");
        });

        Check("teardown", 1, ["lua"], () => {
            Interop.Eval("lua", "_G.at_exit = setmetatable({}, { __gc = function() "
                + $"io.write('ok {Plan} - Interop.Shutdown() tears UGLY down (a Lua finalizer ran)\\n') io.stdout:flush() end }})");
            Ok(count == Plan - 2, "every test before teardown ran");
        });
        Console.Out.Flush();
        Interop.Shutdown();
        return 0;
    }
}
