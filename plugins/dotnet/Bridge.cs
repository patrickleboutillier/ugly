// The C# side of the .NET plugin. dotnet.c gets each entry point with
// load_assembly_and_get_function_pointer, as an [UnmanagedCallersOnly] method.
// The protocol will live here, as the Java plugin's lives in Bridge.java.
using System.Runtime.InteropServices;

namespace Ugly;

public static class Bridge {
    // Must match BRIDGE_ABI in dotnet.c.
    const int Abi = 1;

    // Called once, from the plugin's init. 0 if the two sides agree.
    [UnmanagedCallersOnly]
    public static int Init(int abi) => abi == Abi ? 0 : -1;
}
