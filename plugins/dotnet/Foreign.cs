// Values from other languages, as C# sees them, and the errors they raise.
using System.Dynamic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace Ugly;

/*
 * A value from another language: one owned UGLY handle, released when this
 * object is collected. There is one Foreign per foreign value (== holds).
 * Its operations go through the core with that language's own rules. It is
 * also a DynamicObject: through 'dynamic', f.m(x, name: y) invokes m with a
 * keyword argument, f(x) calls it, f.x and f[i] read members and elements.
 */
public sealed unsafe class Foreign : DynamicObject, IEnumerable<object?> {
    static readonly Dictionary<ulong, WeakReference<Foreign>> cache = new();

    internal readonly ulong Handle;

    Foreign(ulong handle) { Handle = handle; }

    ~Foreign() {
        lock (cache) {
            if (cache.TryGetValue(Handle, out WeakReference<Foreign>? w) && !w.TryGetTarget(out _)) cache.Remove(Handle);
        }
        Core.ugly_release(0, Handle);     // from the finalizer thread: the core defers it to the home runtime
    }

    // The Foreign for an owned handle: the cached one (releasing the extra reference), or a new one.
    internal static Foreign Of(ulong handle) {
        Foreign? f;
        lock (cache) {
            if (!(cache.TryGetValue(handle, out WeakReference<Foreign>? w) && w.TryGetTarget(out f))) {
                f = new Foreign(handle);
                cache[handle] = new WeakReference<Foreign>(f);
                return f;
            }
        }
        Core.ugly_release(Ops.Ctx(), handle);     // never under our lock: a release may run another language's code
        return f;
    }

    int traits = -1;     // cached by IsClass

    // A class that can be instantiated but not called: calling it constructs it, as calling a class does in C#'s neighbours.
    bool IsClass {
        get {
            if (traits < 0) traits = (int)Ops.Traits(Handle);
            return ((traits & Protocol.INSTANTIABLE) != 0) && ((traits & Protocol.CALLABLE) == 0);
        }
    }

    internal object? CallWith(object?[] args, Kw? kw) => IsClass ? Ops.New(Handle, args, kw) : Ops.Call(Handle, null, args, kw);

    /** Calls it (or constructs it, for a class that can only be instantiated); an Interop.Kw or Interop.Opts last gives call options. */
    public object? Call(params object?[] args) => CallWith(args, null);

    /** Invokes a method, with the foreign language's receiver rules; an Interop.Kw or Interop.Opts last gives call options. */
    public object? Invoke(string method, params object?[] args) => Ops.Call(Handle, method, args, null);

    /** Instantiates it (a class), with the foreign language's rules. */
    public object? New(params object?[] args) => Ops.New(Handle, args, null);

    public object? Get(string member) => Ops.Get(Handle, member);
    public void Set(string member, object? value) => Ops.Set(Handle, member, value);

    int Kinds { get { if (traits < 0) traits = (int)Ops.Traits(Handle); return traits; } }

    /** Whether it is a keyed collection (a dict, a table, a Map): contract §2, "Entries". */
    public bool HasEntries => (Kinds & Protocol.ENTRIES) != 0;

    // p[key]: an element, for an integer on a sequence; otherwise the entry of a keyed
    // collection; otherwise, for a string, the member of that name (p["name"] as p.name).
    bool Keyed(object? key) => HasEntries && !(((Kinds & Protocol.ELEMENTS) != 0) && (key is int or long or short or byte));

    object? At(object? key) {
        if (Keyed(key)) return GetEntry(key);
        if (key is string s) return Get(s);
        return Ops.GetIndex(Handle, System.Convert.ToInt64(key));
    }

    void Put(object? key, object? value) {
        if (Keyed(key)) SetEntry(key, value);
        else if (key is string s) Set(s, value);
        else Ops.SetIndex(Handle, System.Convert.ToInt64(key), value);
    }

    public object? this[int index] {
        get => At(index);
        set => Put(index, value);
    }

    /** An entry by any key (a KeyNotFoundException if it isn't there), or a member by name where the value has no entries. */
    public object? this[object? key] {
        get => At(key);
        set => Put(key, value);
    }

    /** The entry under key; a KeyNotFoundException if it isn't there. */
    public object? GetEntry(object? key) =>
        Ops.GetEntry(Handle, key, out object? v) ? v : throw new KeyNotFoundException($"ugly: no entry {key}");
    /** The entry under key, if it is there. */
    public bool TryGetEntry(object? key, out object? value) => Ops.GetEntry(Handle, key, out value);
    public void SetEntry(object? key, object? value) => Ops.SetEntry(Handle, key, value);
    /** Removes the entry under key; one that isn't there is not an error. */
    public void DeleteEntry(object? key) => Ops.DeleteEntry(Handle, key);
    public long EntryCount => Ops.EntryCount(Handle);
    /** The keys, as they are now. */
    public List<object?> EntryKeys() => Ops.EntryKeys(Handle);
    public long Length => Ops.Length(Handle);
    public string[] Names => Ops.Names(Handle);
    public uint Traits => Ops.Traits(Handle);

    public override bool Equals(object? o) => (o is Foreign f) && Ops.Same(Handle, f.Handle);
    public override int GetHashCode() => Handle.GetHashCode();

    /** Its own language's string for it (Python's str(), Lua's tostring); Ugly.Foreign(n) for a value it has none for. */
    public override string ToString() => Ops.ToStr(Handle) ?? $"Ugly.Foreign({Handle})";

    /*
     * foreach, and LINQ: the value's own iteration (contract §2), one value
     * per step, so a Python generator runs only as far as the loop goes. A
     * value its language doesn't iterate but that has elements (a Lua table)
     * is walked by index.
     */
    public IEnumerator<object?> GetEnumerator() {
        object? it = Ops.Iter(Handle, out bool refused);
        if (refused) {
            if ((Traits & Protocol.ELEMENTS) == 0) throw new NotSupportedException("ugly: the foreign value is not iterable");
            return Elements();
        }
        return it switch {
            Foreign f => Steps(f),
            System.Collections.IEnumerator e => Native(e),     // its iterator is a C# one, come home
            _ => throw new NotSupportedException("ugly: the foreign value's iterator isn't one"),
        };
    }

    System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() => GetEnumerator();

    static IEnumerator<object?> Steps(Foreign it) {
        while (Ops.Next(it.Handle, out object? v)) yield return v;
    }

    IEnumerator<object?> Elements() {
        long n = Length;
        for (long i = 0; i < n; i++) yield return Ops.GetIndex(Handle, i);
    }

    static IEnumerator<object?> Native(System.Collections.IEnumerator e) {
        while (e.MoveNext()) yield return e.Current;
    }

    /* dynamic */

    static Kw? Named(CallInfo info, object?[] args, out object?[] positional) {
        int n = info.ArgumentNames.Count;
        positional = args[..(args.Length - n)];
        if (n == 0) return null;
        return new Kw(info.ArgumentNames.ToArray(), args[(args.Length - n)..]);
    }

    public override bool TryGetMember(GetMemberBinder binder, out object? result) {
        result = Get(binder.Name);
        return true;
    }

    public override bool TrySetMember(SetMemberBinder binder, object? value) {
        Set(binder.Name, value);
        return true;
    }

    public override bool TryInvokeMember(InvokeMemberBinder binder, object?[]? args, out object? result) {
        Kw? kw = Named(binder.CallInfo, args ?? [], out object?[] a);
        result = Ops.Call(Handle, binder.Name, a, kw);
        return true;
    }

    public override bool TryInvoke(InvokeBinder binder, object?[]? args, out object? result) {
        Kw? kw = Named(binder.CallInfo, args ?? [], out object?[] a);
        result = CallWith(a, kw);
        return true;
    }

    public override bool TryGetIndex(GetIndexBinder binder, object[] indexes, out object? result) {
        result = At(indexes[0]);
        return true;
    }

    public override bool TrySetIndex(SetIndexBinder binder, object[] indexes, object? value) {
        Put(indexes[0], value);
        return true;
    }

    public override IEnumerable<string> GetDynamicMemberNames() => Names;
}


/*
 * An error from another language, thrown into C#: its type, message and
 * language, and the original exception object (Payload, often a Foreign).
 * If it escapes C# unhandled, the original error goes on unchanged.
 */
public sealed class ForeignException : Exception {
    nint err;     // the core's error while this exception owns it

    public string ErrorType { get; }
    public string Language { get; }
    public object? Payload { get; }

    internal ForeignException(nint err, string type, string message, string language, object? payload) : base(message) {
        this.err = err;
        ErrorType = type;
        Language = language;
        Payload = payload;
        if (err == 0) GC.SuppressFinalize(this);
    }

    /** Rethrows a foreign exception object (a Foreign): the payload is that value itself. */
    public ForeignException(object? payload) : this(0, "ForeignError", "rethrown foreign exception", "dotnet", payload) { }

    // Called when the error leaves C#: the core takes it back.
    internal nint Take() {
        nint e = Interlocked.Exchange(ref err, 0);
        if (e != 0) GC.SuppressFinalize(this);
        return e;
    }

    ~ForeignException() {
        nint e = Interlocked.Exchange(ref err, 0);
        if (e != 0) Core.ugly_error_free(e);
    }
}


/* -------------------------------------------------------------------- */
/* Operations on foreign values, through the core                      */
/* -------------------------------------------------------------------- */

static unsafe class Ops {
    // This thread's ctx; a thread that has none gets one, deleted when it ends.
    // C# code is running here, so the releases queued for its runtime run
    // now: they are always queued (any-thread), and a program that only calls
    // out never enters its runtime through the core, where they would run.
    public static nint Ctx() {
        nint ctx;
        Status st = Core.ugly_ctx_thread(&ctx);
        if (st != Status.Ok) throw new InvalidOperationException($"ugly: no context on this thread ({st})");
        nint rt = Bridge.Rt;
        if (rt != 0) Core.ugly_rt_drain(rt);
        return ctx;
    }

    public static nint Runtime(nint ctx, string lang) {
        if (lang == "dotnet") throw new ArgumentException("ugly: use C# itself for .NET");
        nint rt;
        Status st;
        fixed (byte* l = Errors.Z(lang)) st = Core.ugly_rt_get(ctx, l, &rt);
        if (st != Status.Ok) Errors.Throw(ctx, st);
        return rt;
    }

    // A core result as a C# object (and releases it), or throws.
    public static object? Result(nint ctx, Status st, Val* o) {
        if (st != Status.Ok) Errors.Throw(ctx, st);
        try { return Conv.FromVal(ctx, o); }
        finally { Core.ugly_val_release(ctx, o); }
    }

    // C# arguments as owned values, released by Free.
    sealed class Args : IDisposable {
        readonly nint ctx;
        public readonly Val[] Vals;
        public readonly Kwarg[] Kw;
        int made, madeKw;

        public Args(nint ctx, object?[] args, Kw? kw) {
            this.ctx = ctx;
            Vals = new Val[args.Length + 1];
            Kw = new Kwarg[(kw?.Names.Length ?? 0) + 1];
            try {
                fixed (Val* v = Vals) for (; made < args.Length; made++) Conv.ToVal(ctx, args[made], Kind.Any, &v[made]);
                if (kw != null) {
                    fixed (Kwarg* k = Kw) {
                        for (; madeKw < kw.Names.Length; madeKw++) {
                            k[madeKw].Name = (byte*)Marshal.StringToCoTaskMemUTF8(kw.Names[madeKw]);
                            Conv.ToVal(ctx, kw.Values[madeKw], Kind.Any, &k[madeKw].Value);
                        }
                    }
                }
            }
            catch {
                Dispose();
                throw;
            }
        }

        public void Dispose() {
            fixed (Val* v = Vals) for (int i = 0; i < made; i++) Core.ugly_val_release(ctx, &v[i]);
            fixed (Kwarg* k = Kw) {
                for (int i = 0; i <= madeKw && i < Kw.Length; i++) {
                    if (k[i].Name != null) Marshal.FreeCoTaskMem((nint)k[i].Name);
                    k[i].Name = null;
                    if (i < madeKw) Core.ugly_val_release(ctx, &k[i].Value);
                }
            }
            made = madeKw = 0;
        }
    }

    /*
     * The call options at the end of args (an Interop.Kw, an Interop.Opts,
     * in either order), with 'named' (a dynamic call's named arguments)
     * after the Kw's. Returns the positional arguments.
     */
    static object?[] Split(object?[] args, ref Kw? named, out CallOptions? opts) {
        Kw? kw = null;
        opts = null;
        int n = args.Length;
        for (int k = 0; (k < 2) && (n > 0); k++) {
            if ((kw == null) && (args[n - 1] is Kw x)) { kw = x; n--; }
            else if ((opts == null) && (args[n - 1] is CallOptions o)) { opts = o; n--; }
            else break;
        }
        if (kw != null) named = kw.With(named);
        return n == args.Length ? args : args[..n];
    }

    // The native options for a call: kwargs from 'a', hint and context as UTF-8 (freed by the caller).
    static Opts MakeOpts(Kwarg* k, int n, CallOptions? o, out nint hint, out nint context) {
        hint = o?.Hint != null ? Marshal.StringToCoTaskMemUTF8(o.Hint) : 0;
        context = o?.Context != null ? Marshal.StringToCoTaskMemUTF8(o.Context) : 0;
        return new() { Size = (uint)sizeof(Opts), Kwargs = k, NKwargs = (nuint)n, Hint = (byte*)hint, Context = (byte*)context };
    }

    public static object? Call(ulong h, string? method, object?[] args, Kw? kw) {
        args = Split(args, ref kw, out CallOptions? co);
        nint ctx = Ctx();
        using var a = new Args(ctx, args, kw);
        Val o;
        Status st;
        nint hint = 0, context = 0;
        try {
            fixed (Val* v = a.Vals)
            fixed (Kwarg* k = a.Kw) {
                Opts opts = MakeOpts(k, kw?.Names.Length ?? 0, co, out hint, out context);
                if (method == null) st = Core.ugly_call_opts(ctx, h, v, (nuint)args.Length, &opts, Kind.Any, &o);
                else fixed (byte* m = Errors.Z(method)) st = Core.ugly_invoke_opts(ctx, h, m, v, (nuint)args.Length, &opts, Kind.Any, &o);
            }
        }
        finally {
            Marshal.FreeCoTaskMem(hint);
            Marshal.FreeCoTaskMem(context);
        }
        return Result(ctx, st, &o);
    }

    public static object? New(ulong h, object?[] args, Kw? kw) {
        args = Split(args, ref kw, out CallOptions? co);
        nint ctx = Ctx();
        using var a = new Args(ctx, args, kw);
        ulong r;
        Status st;
        nint hint = 0, context = 0;
        try {
            fixed (Val* v = a.Vals)
            fixed (Kwarg* k = a.Kw) {
                Opts opts = MakeOpts(k, kw?.Names.Length ?? 0, co, out hint, out context);
                st = Core.ugly_new_opts(ctx, h, v, (nuint)args.Length, &opts, &r);
            }
        }
        finally {
            Marshal.FreeCoTaskMem(hint);
            Marshal.FreeCoTaskMem(context);
        }
        if (st != Status.Ok) Errors.Throw(ctx, st);
        return Foreign.Of(r);
    }

    public static object? Get(ulong h, string member) {
        nint ctx = Ctx();
        Val o;
        Status st;
        fixed (byte* m = Errors.Z(member)) st = Core.ugly_get_member(ctx, h, m, Kind.Any, &o);
        return Result(ctx, st, &o);
    }

    public static void Set(ulong h, string member, object? value) {
        nint ctx = Ctx();
        using var a = new Args(ctx, [value], null);
        Status st;
        fixed (byte* m = Errors.Z(member)) st = Core.ugly_set_member(ctx, h, m, a.Vals[0]);
        if (st != Status.Ok) Errors.Throw(ctx, st);
    }

    // Entries (contract §2): a keyed collection's, by any key.
    public static bool GetEntry(ulong h, object? key, out object? value) {
        nint ctx = Ctx();
        using var a = new Args(ctx, [key], null);
        Val o;
        Status st = Core.ugly_get_entry(ctx, h, a.Vals[0], Kind.Any, &o);
        value = null;
        if (st == Status.NotFound) return false;
        value = Result(ctx, st, &o);
        return true;
    }

    public static void SetEntry(ulong h, object? key, object? value) {
        nint ctx = Ctx();
        using var a = new Args(ctx, [key, value], null);
        Status st = Core.ugly_set_entry(ctx, h, a.Vals[0], a.Vals[1]);
        if (st != Status.Ok) Errors.Throw(ctx, st);
    }

    public static void DeleteEntry(ulong h, object? key) {
        nint ctx = Ctx();
        using var a = new Args(ctx, [key], null);
        Status st = Core.ugly_delete_entry(ctx, h, a.Vals[0]);
        if (st != Status.Ok) Errors.Throw(ctx, st);
    }

    public static long EntryCount(ulong h) {
        nint ctx = Ctx();
        long n;
        Status st = Core.ugly_entry_count(ctx, h, &n);
        if (st != Status.Ok) Errors.Throw(ctx, st);
        return n;
    }

    public static List<object?> EntryKeys(ulong h) {
        nint ctx = Ctx();
        ulong it;
        Status st = Core.ugly_entry_keys(ctx, h, &it);
        if (st != Status.Ok) Errors.Throw(ctx, st);
        var keys = new List<object?>();
        try {
            for (;;) {
                Val k;
                nuint got = 0;
                st = Core.ugly_next(ctx, it, Kind.Any, &k, 1, &got);
                if (st != Status.Ok) Errors.Throw(ctx, st);
                if (got == 0) break;
                keys.Add(Result(ctx, Status.Ok, &k));
            }
        }
        finally { Core.ugly_release(ctx, it); }
        return keys;
    }

    public static object? GetIndex(ulong h, long i) {
        nint ctx = Ctx();
        Val o;
        Status st = Core.ugly_get_index(ctx, h, i, Kind.Any, &o);
        return Result(ctx, st, &o);
    }

    public static void SetIndex(ulong h, long i, object? value) {
        nint ctx = Ctx();
        using var a = new Args(ctx, [value], null);
        Status st = Core.ugly_set_index(ctx, h, i, a.Vals[0]);
        if (st != Status.Ok) Errors.Throw(ctx, st);
    }

    public static long Length(ulong h) {
        nint ctx = Ctx();
        long n;
        Status st = Core.ugly_length(ctx, h, &n);
        if (st != Status.Ok) Errors.Throw(ctx, st);
        return n;
    }

    public static string[] Names(ulong h) {
        nint ctx = Ctx();
        ulong list;
        Status st = Core.ugly_member_names(ctx, h, &list);
        if (st != Status.Ok) Errors.Throw(ctx, st);
        try {
            long n;
            Core.ugly_length(ctx, list, &n);
            var names = new string[n];
            for (long i = 0; i < n; i++) {
                Val s;
                if (Core.ugly_get_index(ctx, list, i, Kind.String, &s) == Status.Ok) names[i] = (string)Result(ctx, Status.Ok, &s)!;
                else names[i] = "";
            }
            return names;
        }
        finally { Core.ugly_release(ctx, list); }
    }

    public static uint Traits(ulong h) {
        nint ctx = Ctx();
        uint traits = 0;
        Val name;
        Status st = Core.ugly_describe(ctx, h, &traits, &name);
        if (st != Status.Ok) Errors.Throw(ctx, st);
        Core.ugly_val_release(ctx, &name);
        return traits;
    }

    // Its iterator (a Foreign, or a C# enumerator come home); refused if its language doesn't iterate it.
    public static object? Iter(ulong h, out bool refused) {
        nint ctx = Ctx();
        ulong it;
        Status st = Core.ugly_iter(ctx, h, &it);
        refused = (st == Status.NotCapable) || (st == Status.NotImplemented);
        if (refused) return null;
        if (st != Status.Ok) Errors.Throw(ctx, st);
        Val v = new() { Kind = Kind.Handle, H = it };
        return Result(ctx, Status.Ok, &v);
    }

    // One step of a foreign iterator: false at the end.
    public static bool Next(ulong it, out object? value) {
        nint ctx = Ctx();
        Val o;
        nuint got;
        Status st = Core.ugly_next(ctx, it, Kind.Any, &o, 1, &got);
        if (st != Status.Ok) Errors.Throw(ctx, st);
        value = got == 1 ? Result(ctx, Status.Ok, &o) : null;
        return got == 1;
    }

    // Its language's string for it, or null if it has none (or the handle is gone).
    public static string? ToStr(ulong h) {
        nint ctx = Ctx();
        Val o;
        Status st = Core.ugly_to_string(ctx, h, &o);
        if (st == Status.Raised) Errors.Throw(ctx, st);
        if (st != Status.Ok) return null;
        if (o.Kind == Kind.Bytes) {
            string s = o.Len == 0 ? "" : System.Text.Encoding.UTF8.GetString(o.Ptr, (int)o.Len);
            Core.ugly_val_release(ctx, &o);
            return s;
        }
        return (string?)Result(ctx, Status.Ok, &o);
    }

    public static bool Same(ulong a, ulong b) {
        byte same = 0;
        Core.ugly_same(Ctx(), a, b, &same);
        return same != 0;
    }
}


/* -------------------------------------------------------------------- */
/* Modules: types, from loaded assemblies or the module path           */
/* -------------------------------------------------------------------- */

static unsafe class Loader {
    static readonly List<string> paths = new();

    // The module path: ugly_configure("dotnet", "path", dir) values, then UGLY_DOTNET_PATH.
    public static void Init() {
        fixed (byte* lang = Errors.Z("dotnet"), key = Errors.Z("path")) {
            byte* v;
            for (nuint i = 0; (v = Core.ugly_option(lang, key, i)) != null; i++) paths.Add(Marshal.PtrToStringUTF8((nint)v)!);
        }
        string? env = Environment.GetEnvironmentVariable("UGLY_DOTNET_PATH");
        if (!string.IsNullOrEmpty(env)) paths.AddRange(env.Split(':', StringSplitOptions.RemoveEmptyEntries));
        // What an assembly from the path references is found on the path too.
        AssemblyLoadContext.Default.Resolving += (alc, name) => {
            foreach (string dir in paths) {
                string file = Path.Combine(dir, name.Name + ".dll");
                if (File.Exists(file)) return alc.LoadFromAssemblyPath(Path.GetFullPath(file));
            }
            return null;
        };
    }

    static Type? Find(string name) {
        foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies()) {
            if (a.GetType(name) is Type t) return t;
        }
        return null;
    }

    /*
     * A type by full name ("System.Math", "conformance"): from an assembly
     * already loaded, else from the assembly named by the longest prefix
     * of the name, found on the module path or among the framework's.
     */
    public static Type Import(string name) {
        if (Find(name) is Type t) return t;
        for (string prefix = name; prefix.Length > 0; prefix = prefix.Contains('.') ? prefix[..prefix.LastIndexOf('.')] : "") {
            foreach (string dir in paths) {
                string file = Path.Combine(dir, prefix + ".dll");
                if (File.Exists(file) && (AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.GetFullPath(file)).GetType(name) is Type found)) {
                    return found;
                }
            }
            try {
                if (Assembly.Load(new AssemblyName(prefix)).GetType(name) is Type found) return found;
            }
            catch (FileNotFoundException) { }
            catch (FileLoadException) { }
        }
        // Last, any assembly on the module path: a type's assembly needn't be named after it.
        foreach (string dir in paths) {
            if (!Directory.Exists(dir)) continue;
            foreach (string file in Directory.EnumerateFiles(dir, "*.dll")) {
                try {
                    if (AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.GetFullPath(file)).GetType(name) is Type found) return found;
                }
                catch (BadImageFormatException) { }     // a native library
                catch (FileLoadException) { }
            }
        }
        throw new Failure(Status.NotFound, "TypeLoadException", $"no type {name}");
    }
}
