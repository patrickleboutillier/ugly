// The .NET binding: a .NET program using the other languages. Ugly.dll is
// the whole binding: the CLR is already running, so there's nothing native
// to load but libugly itself. On the first use of Interop, this registers
// the plugin (the same [UnmanagedCallersOnly] entry points dotnet.c gives
// the core, in a vtable built here) and attaches it to the running CLR, on
// that thread. Interop.Shutdown() tears down, on that same thread: .NET's
// ProcessExit runs on another one, which owns no ctx.
using System.Reflection;
using System.Runtime.InteropServices;

namespace Ugly;

/** Keyword arguments, for the last argument of a call: f.Invoke("dumps", data, Interop.Kw("indent", 2)). */
public sealed class Kw {
    internal string[] Names { get; }
    internal object?[] Values { get; }

    internal Kw(string[] names, object?[] values) {
        Names = names;
        Values = values;
    }

    // These first, then 'more' (named arguments of a dynamic call).
    internal Kw With(Kw? more) => more == null ? this : new Kw([.. Names, .. more.Names], [.. Values, .. more.Values]);
}

/** Call options other than keyword arguments, for the last argument of a call (beside a Kw, in either order). */
public sealed class CallOptions {
    public string? Hint { get; }
    public string? Context { get; }

    internal CallOptions(string? hint, string? context) {
        Hint = hint;
        Context = context;
    }
}


/** UGLY from C#: other languages' modules, call options, and teardown. */
public static unsafe class Interop {
    public static bool IsForeign(object? o) => Conv.ForeignOf(o) != null;

    /** Module 'name' of language 'lang' ("python", "json"), in this thread's runtime of it. */
    public static Foreign Module(string lang, string name) {
        Binding.Ensure();
        nint ctx = Ops.Ctx();
        nint rt = Ops.Runtime(ctx, lang);
        ulong h;
        Status st;
        fixed (byte* n = Errors.Z(name)) st = Core.ugly_rt_import(ctx, rt, n, &h);
        if (st != Status.Ok) Errors.Throw(ctx, st);
        return Foreign.Of(h);
    }

    /** Evaluates code in language 'lang' (if it can). */
    public static object? Eval(string lang, string code) {
        Binding.Ensure();
        nint ctx = Ops.Ctx();
        nint rt = Ops.Runtime(ctx, lang);
        byte[] c = System.Text.Encoding.UTF8.GetBytes(code);
        Val o;
        Status st;
        fixed (byte* p = c) st = Core.ugly_rt_eval(ctx, rt, p, (nuint)c.Length, Kind.Any, &o);
        return Ops.Result(ctx, st, &o);
    }

    /**
     * Options for a language that hasn't started: Configure("python", "path", "./py"),
     * Configure("java", "options", "-Xmx64m"). Each value is appended; none clears the key.
     */
    public static void Configure(string lang, string key, params string[] values) {
        Binding.Library();
        nint ctx = Ops.Ctx();
        fixed (byte* l = Errors.Z(lang), k = Errors.Z(key)) {
            if (values.Length == 0) Check(ctx, Core.ugly_configure(ctx, l, k, null));
            foreach (string v in values) {
                fixed (byte* p = Errors.Z(v)) Check(ctx, Core.ugly_configure(ctx, l, k, p));
            }
        }
    }

    /** Keyword arguments as name, value pairs: Interop.Kw("indent", 2, "sort_keys", true). */
    public static global::Ugly.Kw Kw(params object?[] pairs) {
        if (pairs.Length % 2 != 0) throw new ArgumentException("Interop.Kw: expected name, value pairs");
        var names = new string[pairs.Length / 2];
        var values = new object?[pairs.Length / 2];
        for (int i = 0; i < names.Length; i++) {
            names[i] = pairs[2 * i] as string ?? throw new ArgumentException($"Interop.Kw: argument {2 * i} isn't a name");
            values[i] = pairs[2 * i + 1];
        }
        return new global::Ugly.Kw(names, values);
    }

    /** Keyword arguments from a dictionary, in its order. */
    public static global::Ugly.Kw Kw(IEnumerable<KeyValuePair<string, object?>> pairs) =>
        new(pairs.Select(p => p.Key).ToArray(), pairs.Select(p => p.Value).ToArray());

    /** A hint (Java's or C#'s overload) or a context (Perl's "scalar"...), for the last argument of a call. */
    public static CallOptions Opts(string? hint = null, string? context = null) => new(hint, context);

    /**
     * Tears UGLY down: this program's runtime, and the other languages'
     * runtimes this thread got (their finalizers run). Call it on the thread
     * that first used UGLY, before the program ends; without it, the process
     * just ends. Using Interop again afterwards starts over.
     */
    public static void Shutdown() => Binding.Shutdown();

    static void Check(nint ctx, Status st) {
        if (st != Status.Ok) Errors.Throw(ctx, st);
    }
}


/* -------------------------------------------------------------------- */
/* Registering and attaching                                            */
/* -------------------------------------------------------------------- */

// ugly_plugin (ugly_plugin.h), field for field.
[StructLayout(LayoutKind.Sequential)]
unsafe struct PluginTable {
    public uint Abi, Caps;
    public byte* Name;
    public nint Init, Term, RtNew, RtAttach, RtDelete, Import, Globals, Eval, Describe, GetMember, SetMember,
        MemberNames, Length, GetIndex, SetIndex, Call, Invoke, NewInstance, DeleteMember, Splice, Release;
    public byte** Options;
    public nint CallOpts, InvokeOpts, NewOpts;
    public nint Iter, Next, ToStr;     // ABI 5
}

static unsafe class Binding {
    const uint PluginAbi = 5;     // UGLY_PLUGIN_ABI
    const uint CapAttach = 1u << 2, CapAnyThread = 1u << 4;

    static readonly object gate = new();
    static bool resolved;
    static nint lang;
    static nint attachCtx;
    static int attachThread;
    static PluginTable* table;

    // Embedded (dotnet.c started the CLR): the plugin, not the binding.
    internal static bool Embedded;

    // The P/Invoke library "ugly": libugly, already in the process when embedded.
    internal static void UseLibrary(nint handle) {
        lock (gate) {
            if (resolved) return;
            NativeLibrary.SetDllImportResolver(typeof(Binding).Assembly,
                (name, asm, path) => name == "ugly" ? handle : IntPtr.Zero);
            resolved = true;
        }
    }

    /*
     * libugly for a .NET program: UGLY_LIBRARY (a path), else libugly.so.2
     * by the system's search (LD_LIBRARY_PATH, the installed libdir), else
     * next to Ugly.dll.
     */
    internal static void Library() {
        if (Volatile.Read(ref resolved)) return;
        string? explicitPath = Environment.GetEnvironmentVariable("UGLY_LIBRARY");
        nint h;
        if (!string.IsNullOrEmpty(explicitPath)) {
            h = NativeLibrary.Load(explicitPath);
        }
        else if (!NativeLibrary.TryLoad("libugly.so.2", out h)) {
            string beside = Path.Combine(Path.GetDirectoryName(typeof(Binding).Assembly.Location) ?? ".", "libugly.so.2");
            if (!NativeLibrary.TryLoad(beside, out h)) {
                throw new DllNotFoundException("ugly: no libugly.so.2 (set UGLY_LIBRARY to its path, or LD_LIBRARY_PATH to its directory)");
            }
        }
        UseLibrary(h);
    }

    // Registers the plugin and attaches it to this CLR, on this thread, if it isn't yet.
    internal static void Ensure() {
        if (Embedded || (Volatile.Read(ref Bridge.Rt) != 0)) return;
        lock (gate) {
            if (Embedded || (Bridge.Rt != 0)) return;
            Library();
            nint ctx = Ops.Ctx();
            nint l = lang;
            if (l == 0) {
                if (table == null) {
                    table = MakeTable();
                    Loader.Init();
                }
                Status st = Core.ugly_lang_register(ctx, table, &l);
                if (st != Status.Ok) Errors.Throw(ctx, st);
                lang = l;
            }
            nint rt;
            Status s = Core.ugly_rt_attach(ctx, l, (nint)table, &rt);     // RtAttach sets Bridge.Rt
            if (s != Status.Ok) Errors.Throw(ctx, s);
            attachCtx = ctx;
            attachThread = Environment.CurrentManagedThreadId;
        }
    }

    internal static void Shutdown() {
        lock (gate) {
            if (Embedded || (Bridge.Rt == 0)) return;
            if (Environment.CurrentManagedThreadId != attachThread) {
                throw new InvalidOperationException("ugly: Interop.Shutdown() belongs on the thread that first used UGLY");
            }
            nint ctx = attachCtx;
            Status st = Core.ugly_rt_delete(ctx, Bridge.Rt);
            if (st != Status.Ok) Errors.Throw(ctx, st);
            Core.ugly_lang_unload(ctx, lang);
            lang = 0;
            Core.ugly_ctx_delete(ctx);     // and the runtimes Interop.Module created on this thread
            attachCtx = 0;
        }
    }

    static PluginTable* MakeTable() {
        var t = (PluginTable*)NativeMemory.AllocZeroed((nuint)sizeof(PluginTable));     // for the life of the process
        t->Abi = PluginAbi;
        t->Caps = CapAttach | CapAnyThread;
        t->Name = (byte*)Marshal.StringToCoTaskMemUTF8("dotnet");
        t->Options = (byte**)NativeMemory.AllocZeroed(2, (nuint)sizeof(byte*));
        t->Options[0] = (byte*)Marshal.StringToCoTaskMemUTF8("path");
        t->RtNew = (nint)(delegate* unmanaged<nint, nint, nint*, int>)&Bridge.RtNew;
        t->RtAttach = (nint)(delegate* unmanaged<nint, nint, nint, nint*, int>)&Bridge.RtAttach;
        t->RtDelete = (nint)(delegate* unmanaged<nint, nint, nint, int>)&Bridge.RtDelete;
        t->Import = (nint)(delegate* unmanaged<nint, nint, byte*, Val*, int>)&Bridge.Import;
        t->Describe = (nint)(delegate* unmanaged<nint, nint, nint, uint*, Val*, int>)&Bridge.Describe;
        t->GetMember = (nint)(delegate* unmanaged<nint, nint, nint, byte*, int, Val*, int>)&Bridge.GetMember;
        t->SetMember = (nint)(delegate* unmanaged<nint, nint, nint, byte*, Val, int>)&Bridge.SetMember;
        t->MemberNames = (nint)(delegate* unmanaged<nint, nint, nint, ulong*, int>)&Bridge.MemberNames;
        t->Length = (nint)(delegate* unmanaged<nint, nint, nint, long*, int>)&Bridge.Length;
        t->GetIndex = (nint)(delegate* unmanaged<nint, nint, nint, long, int, Val*, int>)&Bridge.GetIndex;
        t->SetIndex = (nint)(delegate* unmanaged<nint, nint, nint, long, Val, int>)&Bridge.SetIndex;
        t->Call = (nint)(delegate* unmanaged<nint, nint, nint, Val*, nuint, int, Val*, int>)&Bridge.Call;
        t->Invoke = (nint)(delegate* unmanaged<nint, nint, nint, byte*, Val*, nuint, int, Val*, int>)&Bridge.Invoke;
        t->NewInstance = (nint)(delegate* unmanaged<nint, nint, nint, Val*, nuint, ulong*, int>)&Bridge.New;
        t->DeleteMember = (nint)(delegate* unmanaged<nint, nint, nint, byte*, int>)&Bridge.DeleteMember;
        t->Splice = (nint)(delegate* unmanaged<nint, nint, nint, long, long, Val*, nuint, int>)&Bridge.Splice;
        t->Release = (nint)(delegate* unmanaged<nint, nint, void>)&Bridge.Release;
        t->CallOpts = (nint)(delegate* unmanaged<nint, nint, nint, Val*, nuint, Opts*, int, Val*, int>)&Bridge.CallOpts;
        t->InvokeOpts = (nint)(delegate* unmanaged<nint, nint, nint, byte*, Val*, nuint, Opts*, int, Val*, int>)&Bridge.InvokeOpts;
        t->NewOpts = (nint)(delegate* unmanaged<nint, nint, nint, Val*, nuint, Opts*, ulong*, int>)&Bridge.NewOpts;
        t->Iter = (nint)(delegate* unmanaged<nint, nint, nint, ulong*, int>)&Bridge.Iter;
        t->Next = (nint)(delegate* unmanaged<nint, nint, nint, int, Val*, nuint, nuint*, int>)&Bridge.Next;
        t->ToStr = (nint)(delegate* unmanaged<nint, nint, nint, Val*, int>)&Bridge.ToStr;
        return t;
    }
}
