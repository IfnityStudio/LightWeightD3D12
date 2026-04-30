using System.Runtime.InteropServices;
using System.Text;

namespace HelloWpfTriangle;

internal static partial class NativeMethods
{
    private const string NativeDll = "LightD3D12WpfNative.dll";

    [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr LightWpf_Create(IntPtr parentHwnd, uint width, uint height);

    [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void LightWpf_Destroy(IntPtr nativeContext);

    [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr LightWpf_GetChildWindow(IntPtr nativeContext);

    [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int LightWpf_Resize(IntPtr nativeContext, uint width, uint height);

    [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int LightWpf_Render(IntPtr nativeContext);

    [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void LightWpf_SetClearColor(IntPtr nativeContext, float red, float green, float blue);

    [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void LightWpf_SetAnimationSpeed(IntPtr nativeContext, float speed);

    [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
    private static extern int LightWpf_GetLastError(StringBuilder? buffer, int capacity);

    internal static string GetLastError()
    {
        var builder = new StringBuilder(1024);
        _ = LightWpf_GetLastError(builder, builder.Capacity);
        return builder.ToString();
    }
}
