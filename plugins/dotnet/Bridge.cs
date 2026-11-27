// The C# side of the .NET plugin. dotnet.c starts the CLR, loads this
// assembly into the default load context, and fills the plugin's vtable
// with the [UnmanagedCallersOnly] entry points below, so the core calls C#
// directly. The protocol itself (reflection, overloads) is in Protocol.cs;
// this file converts values, keeps native refs, and maps exceptions.
//
// - A native ref is a GCHandle, one per object, counted (Refs). Its address
//   is also the object's identity for the core.
// - Values: integers -> INT (ulong, BigInteger... if they fit), double and
//   float -> DOUBLE (integers widen to DOUBLE on request), string and char
//   -> STRING, byte[] -> BYTES, null -> NULL, anything else is a handle.
//   INT comes in as long, and narrows to int, short... parameters when it
//   fits. Strings are converted strictly: no invalid UTF-8 in, no lone
//   surrogate out.
// - An exception escaping to the core is raised with itself as the payload
//   (rule 7); Protocol's NotFound, NotCapable and TypeMismatch become those
//   statuses.
using System.Collections;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Ugly;

public static unsafe class Bridge {
    // Must match BRIDGE_ABI in dotnet.c.
    const int Abi = 3;

    // The runtime: the CLR, from any thread. One at a time (no UGLY_CAP_MULTI_RUNTIME).
    internal static nint Rt;

    // Called once, from the plugin's init, with libugly's dlopen handle. 0 if all is well.
    [UnmanagedCallersOnly]
    public static int Init(int abi, nint lib) {
        if (abi != Abi) return -1;
        try {
            Binding.Embedded = true;
            Binding.UseLibrary(lib);
            Loader.Init();
            return 0;
        }
        catch (Exception e) {
            Console.Error.WriteLine($"ugly dotnet: {e}");
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static int RtNew(nint ctx, nint rt, nint* native) {
        *native = 0;
        Rt = rt;
        return (int)Status.Ok;
    }

    // The binding: this CLR, already running (Binding.Ensure).
    [UnmanagedCallersOnly]
    public static int RtAttach(nint ctx, nint rt, nint existing, nint* native) {
        *native = 0;
        Rt = rt;
        return (int)Status.Ok;
    }

    [UnmanagedCallersOnly]
    public static int RtDelete(nint ctx, nint rt, nint native) {
        if (Rt == rt) Rt = 0;
        return (int)Status.Ok;
    }

    [UnmanagedCallersOnly]
    public static void Release(nint rt, nint r) {
        try { Refs.Release(r); }
        catch (Exception e) { Console.Error.WriteLine($"ugly dotnet: release: {e.Message}"); }
    }

    /* ---------------------------------------------------------------- */
    /* Operations                                                       */
    /* ---------------------------------------------------------------- */

    static void Enter() => RuntimeHelpers.EnsureSufficientExecutionStack();

    static object Obj(nint r) => Refs.Target(r);

    static string Name(byte* s) => Marshal.PtrToStringUTF8((nint)s) ?? "";

    [UnmanagedCallersOnly]
    public static int Import(nint ctx, nint rt, byte* name, Val* o) {
        try {
            Enter();
            Conv.ToVal(ctx, Loader.Import(Name(name)), Kind.Handle, o);
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    [UnmanagedCallersOnly]
    public static int Describe(nint ctx, nint rt, nint r, uint* traits, Val* typeName) {
        try {
            Enter();
            object o = Obj(r);
            *traits = Protocol.Traits(o);
            Conv.ToVal(ctx, Protocol.TypeName(o), Kind.String, typeName);
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    [UnmanagedCallersOnly]
    public static int GetMember(nint ctx, nint rt, nint r, byte* name, int want, Val* o) {
        try {
            Enter();
            Conv.ToVal(ctx, Protocol.GetMember(Obj(r), Name(name)), (Kind)want, o);
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    [UnmanagedCallersOnly]
    public static int SetMember(nint ctx, nint rt, nint r, byte* name, Val v) {
        try {
            Enter();
            Protocol.SetMember(Obj(r), Name(name), Conv.FromVal(ctx, &v));
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    [UnmanagedCallersOnly]
    public static int DeleteMember(nint ctx, nint rt, nint r, byte* name) {
        try {
            Enter();
            Protocol.DeleteMember(Obj(r), Name(name));
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    [UnmanagedCallersOnly]
    public static int MemberNames(nint ctx, nint rt, nint r, ulong* o) {
        try {
            Enter();
            *o = Conv.List(ctx, Protocol.MemberNames(Obj(r)));
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    [UnmanagedCallersOnly]
    public static int Length(nint ctx, nint rt, nint r, long* o) {
        try {
            Enter();
            *o = Protocol.Length(Obj(r));
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    [UnmanagedCallersOnly]
    public static int GetIndex(nint ctx, nint rt, nint r, long i, int want, Val* o) {
        try {
            Enter();
            Conv.ToVal(ctx, Protocol.GetIndex(Obj(r), i), (Kind)want, o);
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    [UnmanagedCallersOnly]
    public static int SetIndex(nint ctx, nint rt, nint r, long i, Val v) {
        try {
            Enter();
            Protocol.SetIndex(Obj(r), i, Conv.FromVal(ctx, &v));
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    [UnmanagedCallersOnly]
    public static int Splice(nint ctx, nint rt, nint r, long offset, long remove, Val* insert, nuint n) {
        try {
            Enter();
            Protocol.Splice(Obj(r), offset, remove, Conv.FromVals(ctx, insert, n));
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    [UnmanagedCallersOnly]
    public static int Call(nint ctx, nint rt, nint r, Val* args, nuint n, int want, Val* o) =>
        DoCall(ctx, r, null, args, n, null, want, o);

    [UnmanagedCallersOnly]
    public static int CallOpts(nint ctx, nint rt, nint r, Val* args, nuint n, Opts* opts, int want, Val* o) =>
        DoCall(ctx, r, null, args, n, opts, want, o);

    [UnmanagedCallersOnly]
    public static int Invoke(nint ctx, nint rt, nint r, byte* method, Val* args, nuint n, int want, Val* o) =>
        DoCall(ctx, r, method, args, n, null, want, o);

    [UnmanagedCallersOnly]
    public static int InvokeOpts(nint ctx, nint rt, nint r, byte* method, Val* args, nuint n, Opts* opts, int want, Val* o) =>
        DoCall(ctx, r, method, args, n, opts, want, o);

    [UnmanagedCallersOnly]
    public static int New(nint ctx, nint rt, nint r, Val* args, nuint n, ulong* o) =>
        DoNew(ctx, r, args, n, null, o);

    [UnmanagedCallersOnly]
    public static int NewOpts(nint ctx, nint rt, nint r, Val* args, nuint n, Opts* opts, ulong* o) =>
        DoNew(ctx, r, args, n, opts, o);

    // Its iterator: an enumerable's GetEnumerator(), an enumerator itself.
    [UnmanagedCallersOnly]
    public static int Iter(nint ctx, nint rt, nint r, ulong* o) {
        try {
            Enter();
            *o = Conv.Export(ctx, Protocol.Iter(Obj(r)));
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    // MoveNext() up to max times: a 'yield return' method runs only as far as asked. Values made before an exception are kept.
    [UnmanagedCallersOnly]
    public static int Next(nint ctx, nint rt, nint r, int want, Val* o, nuint max, nuint* got) {
        *got = 0;
        try {
            Enter();
            if (Obj(r) is not IEnumerator e) throw Fail.NotCapable("value is not an enumerator");
            for (nuint i = 0; i < max; i++) {
                if (!e.MoveNext()) break;
                Conv.ToVal(ctx, e.Current, (Kind)want, &o[i]);
                *got = i + 1;
            }
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    // ToString().
    [UnmanagedCallersOnly]
    public static int ToStr(nint ctx, nint rt, nint r, Val* o) {
        try {
            Enter();
            Conv.ToVal(ctx, Obj(r).ToString() ?? "", Kind.String, o);
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    static int DoCall(nint ctx, nint r, byte* method, Val* args, nuint n, Opts* opts, int want, Val* o) {
        try {
            Enter();
            object?[] a = Conv.FromVals(ctx, args, n);
            Kw? kw = TakeOpts(ctx, opts);
            object? res;
            try {
                res = method == null ? Protocol.Call(Obj(r), a, kw) : Protocol.Invoke(Obj(r), Name(method), a, kw);
            }
            finally { Protocol.Hint = null; }
            Conv.ToVal(ctx, res, (Kind)want, o);
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    static int DoNew(nint ctx, nint r, Val* args, nuint n, Opts* opts, ulong* o) {
        try {
            Enter();
            object?[] a = Conv.FromVals(ctx, args, n);
            Kw? kw = TakeOpts(ctx, opts);
            object res;
            try { res = Protocol.New(Obj(r), a, kw); }
            finally { Protocol.Hint = null; }
            Val v;
            Conv.ToVal(ctx, res, Kind.Handle, &v);
            *o = v.H;
            return 0;
        }
        catch (Exception e) { return Errors.Fail(ctx, e); }
    }

    /*
     * Call options (contract §11): keyword arguments bind to parameters by
     * name, as C#'s named arguments do; a hint names the overload's
     * parameter types; there's no call context.
     */
    static Kw? TakeOpts(nint ctx, Opts* opts) {
        if (opts == null) return null;
        if (opts->Context != null) throw new Failure(Status.NotSupported, "NotSupportedException", "C# takes no call context");
        Kw? kw = null;
        int n = (int)opts->NKwargs;
        if (n > 0) {
            kw = new Kw(new string[n], new object?[n]);
            for (int i = 0; i < n; i++) {
                kw.Names[i] = Name(opts->Kwargs[i].Name);
                kw.Values[i] = Conv.FromVal(ctx, &opts->Kwargs[i].Value);
            }
        }
        Protocol.Hint = opts->Hint != null ? Name(opts->Hint) : null;     // last: nothing above may leave it set
        return kw;
    }
}


/* -------------------------------------------------------------------- */
/* Native refs                                                          */
/* -------------------------------------------------------------------- */

// One GCHandle per object, counted: the ref handed to the core, and its identity.
static class Refs {
    sealed class Entry {
        public GCHandle H;
        public int Count;
    }

    static readonly Dictionary<object, Entry> map = new(ReferenceEqualityComparer.Instance);

    public static nint Acquire(object o) {
        lock (map) {
            if (!map.TryGetValue(o, out Entry? e)) {
                e = new Entry { H = GCHandle.Alloc(o) };
                map[o] = e;
            }
            e.Count++;
            return GCHandle.ToIntPtr(e.H);
        }
    }

    // The last release of an enumerator UGLY handed out (Protocol.Iter) disposes of it, as a break out of foreach would.
    public static void Release(nint r) {
        object? last = null;
        lock (map) {
            object? o = GCHandle.FromIntPtr(r).Target;
            if ((o != null) && map.TryGetValue(o, out Entry? e) && (--e.Count <= 0)) {
                map.Remove(o);
                e.H.Free();
                last = o;
            }
        }
        if ((last is IDisposable d) && Protocol.Iterating(last)) d.Dispose();     // outside the lock: it runs C# code (finally blocks)
    }

    public static object Target(nint r) => GCHandle.FromIntPtr(r).Target!;
}


/* -------------------------------------------------------------------- */
/* Failures                                                             */
/* -------------------------------------------------------------------- */

// A failure that isn't a C# exception escaping: becomes its status, with no payload.
sealed class Failure(Status status, string type, string message) : Exception(message) {
    public Status Status { get; } = status;
    public string TypeName { get; } = type;
}

// A call into the core failed, and the core has recorded the error in ctx already.
sealed class CoreStatus(Status status) : Exception($"ugly: {status}") {
    public Status Status { get; } = status;
}

static class Fail {
    public static Failure NotFound(string what) => new(Status.NotFound, "MissingMemberException", what);
    public static Failure NotCapable(string what) => new(Status.NotCapable, "NotSupportedException", what);
    public static Failure Type(string what) => new(Status.Type, "InvalidCastException", what);
}


static unsafe class Errors {
    public static byte[] Z(string s) => Encoding.UTF8.GetBytes(s + "\0");

    public static void Check(Status st) {
        if (st != Status.Ok) throw new CoreStatus(st);
    }

    // An exception escaping to the core (rule 7). Returns the status.
    public static int Fail(nint ctx, Exception e) {
        switch (e) {
            case CoreStatus c:
                return (int)c.Status;
            case Failure f:
                fixed (byte* t = Z(f.TypeName), m = Z(f.Message)) return (int)Core.ugly_fail(ctx, Bridge.Rt, f.Status, t, m);
            case ForeignException fe: {
                nint err = fe.Take();
                if (err != 0) return (int)Core.ugly_restore_error(ctx, err);     // ours, unchanged
                // new ForeignException(payload): rethrowing a foreign value, which is the payload itself.
                if (Conv.ForeignOf(fe.Payload) is Foreign p) {
                    ulong h;
                    Check(Core.ugly_dup(ctx, p.Handle, &h));
                    fixed (byte* t = Z("ForeignError"), m = Z("rethrown foreign value")) {
                        return (int)Core.ugly_raise_handle(ctx, Bridge.Rt, t, m, h);
                    }
                }
                break;
            }
        }
        // A C# exception: its type name, its message, and itself as the payload.
        nint r = Refs.Acquire(e);
        fixed (byte* t = Z(e.GetType().Name), m = Z(e.Message)) {
            return (int)Core.ugly_raise_native(ctx, Bridge.Rt, t, m, r, (nuint)r);
        }
    }

    /*
     * The ctx's current error, thrown into C#: the original exception if
     * its payload is a C# exception coming home (rule 7), otherwise a
     * ForeignException owning the error.
     */
    [DoesNotReturn]
    public static void Throw(nint ctx, Status st) {
        nint err = Core.ugly_take_error(ctx);
        if (err == 0) throw new InvalidOperationException($"ugly: operation failed ({st})");
        ulong p = 0;
        Core.ugly_error_payload(ctx, err, &p);
        if (p != 0) {
            nint r;
            if ((Core.ugly_resolve(ctx, Bridge.Rt, p, &r) == Status.Ok) && (r != 0) && (Refs.Target(r) is Exception original)) {
                Core.ugly_release(ctx, p);
                Core.ugly_error_free(err);
                ExceptionDispatchInfo.Throw(original);
            }
        }
        object? payload = null;
        if (p != 0) {
            ulong d;
            if (Core.ugly_dup(ctx, p, &d) == Status.Ok) payload = Foreign.Of(d);
            Core.ugly_release(ctx, p);
        }
        throw new ForeignException(err, Str(Core.ugly_error_type(err)), Str(Core.ugly_error_message(err)),
            Str(Core.ugly_error_language(err)), payload);
    }

    static string Str(byte* s) => Marshal.PtrToStringUTF8((nint)s) ?? "";
}


/* -------------------------------------------------------------------- */
/* Values                                                               */
/* -------------------------------------------------------------------- */

static unsafe class Conv {
    static readonly UTF8Encoding Strict = new(false, true);

    // Wrappers made for foreign values (delegates, interface proxies): rule 5 unwraps them.
    static readonly ConditionalWeakTable<object, Foreign> wrappers = new();

    public static void Wrapped(object wrapper, Foreign f) => wrappers.AddOrUpdate(wrapper, f);

    public static Foreign? ForeignOf(object? o) =>
        o is Foreign f ? f : (o != null) && wrappers.TryGetValue(o, out Foreign? w) ? w : null;

    static bool Integral(object o, out long v) {
        switch (o) {
            case long x: v = x; return true;
            case int x: v = x; return true;
            case short x: v = x; return true;
            case sbyte x: v = x; return true;
            case byte x: v = x; return true;
            case ushort x: v = x; return true;
            case uint x: v = x; return true;
            case nint x: v = x; return true;
            default: v = 0; return false;
        }
    }

    // Integers that may not fit in int64.
    static bool Wide(object o, out long v, out bool fits) {
        fits = true;
        v = 0;
        switch (o) {
            case ulong x: fits = x <= long.MaxValue; v = (long)x; return true;
            case nuint x: fits = x <= long.MaxValue; v = (long)x; return true;
            case System.Numerics.BigInteger x:
                fits = (x >= long.MinValue) && (x <= long.MaxValue);
                v = fits ? (long)x : 0;
                return true;
            case Int128 x:
                fits = (x >= long.MinValue) && (x <= long.MaxValue);
                v = fits ? (long)x : 0;
                return true;
            case UInt128 x:
                fits = x <= long.MaxValue;
                v = fits ? (long)x : 0;
                return true;
            default:
                return false;
        }
    }

    static bool Floating(object o, out double d) {
        switch (o) {
            case double x: d = x; return true;
            case float x: d = x; return true;
            case Half x: d = (double)x; return true;
            default: d = 0; return false;
        }
    }

    public static bool IsPrimitive(object? o) =>
        (o == null) || (o is bool) || (o is string) || (o is char) || (o is byte[])
        || Integral(o, out _) || Wide(o, out _, out _) || Floating(o, out _);

    static Exception TypeError(string to) => Fail.Type($"can't convert this C# value to {to}");

    // A C# value as an owned result, converted to 'want' with C#'s rules.
    public static void ToVal(nint ctx, object? o, Kind want, Val* v) {
        *v = default;
        v->Kind = Kind.Null;
        bool integral = (o != null) && Integral(o, out _);
        bool wide = (o != null) && Wide(o, out _, out _);
        bool floating = (o != null) && Floating(o, out _);
        bool text = (o is string) || (o is char);
        if (want == Kind.Any) {
            if (o == null) return;
            if (o is bool) want = Kind.Bool;
            else if (integral || wide) want = Kind.Int;
            else if (floating) want = Kind.Double;
            else if (text) want = Kind.String;
            else if (o is byte[]) want = Kind.Bytes;
            else want = Kind.Handle;
        }
        switch (want) {
            case Kind.Null:
                if (o != null) throw TypeError("null");
                return;
            case Kind.Bool:
                if (o is not bool b) throw TypeError("a boolean");
                v->Kind = Kind.Bool;
                v->B = (byte)(b ? 1 : 0);
                return;
            case Kind.Int: {
                long i;
                if (Integral(o!, out i)) { }
                else if (o != null && Wide(o, out i, out bool fits)) {
                    if (!fits) throw new Failure(Status.Type, "OverflowException", "integer doesn't fit in int64");
                }
                else throw TypeError("an integer");     // no implicit narrowing from double
                v->Kind = Kind.Int;
                v->I = i;
                return;
            }
            case Kind.Double: {
                double d;
                if (o == null) throw TypeError("a double");
                if (Floating(o, out d)) { }
                else if (Integral(o, out long i) || Wide(o, out i, out _)) d = o is System.Numerics.BigInteger bi ? (double)bi : i;     // integers widen
                else throw TypeError("a double");
                v->Kind = Kind.Double;
                v->D = d;
                return;
            }
            case Kind.String: {
                string s = o switch {
                    string x => x,
                    char c => c.ToString(),
                    _ => throw TypeError("a string"),
                };
                byte[] bytes;
                try { bytes = Strict.GetBytes(s); }
                catch (EncoderFallbackException) { throw Fail.Type("string has a lone surrogate"); }
                SetBytes(ctx, v, bytes, true);
                return;
            }
            case Kind.Bytes:
                if (o is not byte[] raw) throw TypeError("bytes");
                SetBytes(ctx, v, raw, false);
                return;
            case Kind.Handle: {
                if (IsPrimitive(o)) throw TypeError("a handle");
                ulong h = Export(ctx, o!);
                v->Kind = Kind.Handle;
                v->H = h;
                return;
            }
            default:
                throw new Failure(Status.Usage, "ArgumentException", $"no such kind {want}");
        }
    }

    static void SetBytes(nint ctx, Val* v, byte[] b, bool text) {
        byte none = 0;
        fixed (byte* p = b) {
            byte* q = b.Length > 0 ? p : &none;
            Errors.Check(text ? Core.ugly_val_set_string(ctx, v, q, (nuint)b.Length)
                : Core.ugly_val_set_bytes(ctx, v, q, (nuint)b.Length));
        }
    }

    // An object as an owned handle: a foreign value as itself (rule 5), else ours, deduplicated by identity.
    public static ulong Export(nint ctx, object o) {
        ulong h;
        if (ForeignOf(o) is Foreign f) {
            Errors.Check(Core.ugly_dup(ctx, f.Handle, &h));
            return h;
        }
        nint r = Refs.Acquire(o);
        Status st = Core.ugly_export(ctx, Bridge.Rt, r, (nuint)r, &h);
        if (st != Status.Ok) {
            Refs.Release(r);
            throw new CoreStatus(st);
        }
        return h;
    }

    // A value (borrowed) as a C# object: ours resolve to the original (rule 5), foreign ones to a Foreign.
    public static object? FromVal(nint ctx, Val* v) {
        switch (v->Kind) {
            case Kind.Null: return null;
            case Kind.Bool: return v->B != 0;
            case Kind.Int: return v->I;
            case Kind.Double: return v->D;
            case Kind.String:
                try { return v->Len == 0 ? "" : Strict.GetString(v->Ptr, (int)v->Len); }
                catch (DecoderFallbackException) { throw Fail.Type("string isn't valid UTF-8"); }
            case Kind.Bytes:
                return v->Len == 0 ? Array.Empty<byte>() : new ReadOnlySpan<byte>(v->Ptr, (int)v->Len).ToArray();
            case Kind.Handle: {
                nint r;
                Errors.Check(Core.ugly_resolve(ctx, Bridge.Rt, v->H, &r));
                if (r != 0) return Refs.Target(r);
                ulong h;
                Errors.Check(Core.ugly_dup(ctx, v->H, &h));
                return Foreign.Of(h);
            }
            default:
                throw new Failure(Status.Usage, "ArgumentException", $"no such kind {v->Kind}");
        }
    }

    public static object?[] FromVals(nint ctx, Val* vals, nuint n) {
        var a = new object?[(int)n];
        for (int i = 0; i < a.Length; i++) a[i] = FromVal(ctx, &vals[i]);
        return a;
    }

    // Strings as a core list.
    public static ulong List(nint ctx, IReadOnlyList<string> items) {
        var vals = new Val[items.Count];
        int made = 0;
        try {
            fixed (Val* p = vals) {
                for (; made < items.Count; made++) ToVal(ctx, items[made], Kind.String, &p[made]);
                ulong h;
                Errors.Check(Core.ugly_list_new(ctx, p, (nuint)items.Count, &h));
                return h;
            }
        }
        finally {
            fixed (Val* p = vals) for (int i = 0; i < made; i++) Core.ugly_val_release(ctx, &p[i]);
        }
    }
}
