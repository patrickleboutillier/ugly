// The core's C API as seen from C#: its value types, laid out as in ugly.h,
// and the functions of libugly, called directly (P/Invoke). The "ugly"
// library is resolved by Bridge.Init to the libugly already in the process.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

[assembly: DisableRuntimeMarshalling]

namespace Ugly;

public enum Kind : int { Any, Null, Bool, Int, Double, String, Bytes, Handle }

public enum Status : int {
    Ok, Raised, Type, NotFound, NotCapable, StaleHandle, WrongThread, InUse, TooDeep,
    NotSupported, NotImplemented, Usage, NoMem, Internal,
}

// ugly_val: kind, then a 16-byte union. Public only because the entry points take it.
[StructLayout(LayoutKind.Explicit, Size = 24)]
public unsafe struct Val {
    [FieldOffset(0)] public Kind Kind;
    [FieldOffset(8)] public byte B;
    [FieldOffset(8)] public long I;
    [FieldOffset(8)] public double D;
    [FieldOffset(8)] public ulong H;
    [FieldOffset(8)] public byte* Ptr;
    [FieldOffset(16)] public nuint Len;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct Kwarg {
    public byte* Name;
    public Val Value;
}

// ugly_opts. The core passes the plugin a full-size struct.
[StructLayout(LayoutKind.Sequential)]
public unsafe struct Opts {
    public uint Size;
    public Kwarg* Kwargs;
    public nuint NKwargs;
    public byte* Hint;
    public byte* Context;
}

static unsafe class Core {
    const string L = "ugly";

    // Plugin services (ugly_plugin.h).
    [DllImport(L)] public static extern Status ugly_export(nint ctx, nint rt, nint r, nuint identity, ulong* o);
    [DllImport(L)] public static extern Status ugly_resolve(nint ctx, nint rt, ulong h, nint* r);
    [DllImport(L)] public static extern Status ugly_fail(nint ctx, nint rt, Status st, byte* type, byte* msg);
    [DllImport(L)] public static extern Status ugly_raise_native(nint ctx, nint rt, byte* type, byte* msg, nint payload, nuint identity);
    [DllImport(L)] public static extern Status ugly_raise_handle(nint ctx, nint rt, byte* type, byte* msg, ulong payload);
    [DllImport(L)] public static extern nint ugly_take_error(nint ctx);
    [DllImport(L)] public static extern Status ugly_restore_error(nint ctx, nint err);
    [DllImport(L)] public static extern void ugly_error_free(nint err);
    [DllImport(L)] public static extern nint ugly_current_ctx();
    [DllImport(L)] public static extern byte* ugly_option(byte* lang, byte* key, nuint i);

    [DllImport(L)] public static extern Status ugly_lang_register(nint ctx, PluginTable* plugin, nint* o);

    // The public API (ugly.h).
    [DllImport(L)] public static extern Status ugly_ctx_thread(nint* o);
    [DllImport(L)] public static extern void ugly_ctx_delete(nint ctx);
    [DllImport(L)] public static extern Status ugly_configure(nint ctx, byte* lang, byte* key, byte* value);
    [DllImport(L)] public static extern Status ugly_lang_unload(nint ctx, nint lang);
    [DllImport(L)] public static extern Status ugly_rt_attach(nint ctx, nint lang, nint native, nint* o);
    [DllImport(L)] public static extern Status ugly_rt_delete(nint ctx, nint rt);
    [DllImport(L)] public static extern void ugly_rt_drain(nint rt);
    [DllImport(L)] public static extern nint ugly_last_error(nint ctx);
    [DllImport(L)] public static extern byte* ugly_error_language(nint err);
    [DllImport(L)] public static extern byte* ugly_error_type(nint err);
    [DllImport(L)] public static extern byte* ugly_error_message(nint err);
    [DllImport(L)] public static extern Status ugly_error_payload(nint ctx, nint err, ulong* o);
    [DllImport(L)] public static extern void ugly_val_release(nint ctx, Val* v);
    [DllImport(L)] public static extern Status ugly_val_set_string(nint ctx, Val* o, byte* p, nuint len);
    [DllImport(L)] public static extern Status ugly_val_set_bytes(nint ctx, Val* o, byte* p, nuint len);
    [DllImport(L)] public static extern Status ugly_dup(nint ctx, ulong h, ulong* o);
    [DllImport(L)] public static extern void ugly_release(nint ctx, ulong h);
    [DllImport(L)] public static extern Status ugly_same(nint ctx, ulong a, ulong b, byte* o);
    [DllImport(L)] public static extern Status ugly_list_new(nint ctx, Val* items, nuint n, ulong* o);
    [DllImport(L)] public static extern Status ugly_describe(nint ctx, ulong h, uint* traits, Val* name);
    [DllImport(L)] public static extern Status ugly_iter(nint ctx, ulong h, ulong* o);
    [DllImport(L)] public static extern Status ugly_next(nint ctx, ulong it, Kind want, Val* o, nuint max, nuint* got);
    [DllImport(L)] public static extern Status ugly_to_string(nint ctx, ulong h, Val* o);
    [DllImport(L)] public static extern Status ugly_get_member(nint ctx, ulong h, byte* name, Kind want, Val* o);
    [DllImport(L)] public static extern Status ugly_set_member(nint ctx, ulong h, byte* name, Val v);
    [DllImport(L)] public static extern Status ugly_member_names(nint ctx, ulong h, ulong* o);
    [DllImport(L)] public static extern Status ugly_length(nint ctx, ulong h, long* o);
    [DllImport(L)] public static extern Status ugly_get_index(nint ctx, ulong h, long i, Kind want, Val* o);
    [DllImport(L)] public static extern Status ugly_set_index(nint ctx, ulong h, long i, Val v);
    [DllImport(L)] public static extern Status ugly_call_opts(nint ctx, ulong h, Val* args, nuint n, Opts* opts, Kind want, Val* o);
    [DllImport(L)] public static extern Status ugly_invoke_opts(nint ctx, ulong h, byte* method, Val* args, nuint n, Opts* opts, Kind want, Val* o);
    [DllImport(L)] public static extern Status ugly_new_opts(nint ctx, ulong h, Val* args, nuint n, Opts* opts, ulong* o);
    [DllImport(L)] public static extern Status ugly_rt_get(nint ctx, byte* lang, nint* o);
    [DllImport(L)] public static extern Status ugly_rt_import(nint ctx, nint rt, byte* name, ulong* o);
    [DllImport(L)] public static extern Status ugly_rt_eval(nint ctx, nint rt, byte* code, nuint len, Kind want, Val* o);
}
