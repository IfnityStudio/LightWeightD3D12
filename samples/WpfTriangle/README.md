# WpfTriangle

This sample shows the smallest useful WPF integration path for `LightD3D12`.

The design is:

```text
WPF C#
  -> LightD3D12Host : HwndHost
    -> LightD3D12WpfNative.dll
      -> LightD3D12 DeviceManager / RenderDevice
        -> D3D12 swapchain inside a child HWND
```

The WPF side owns the interface.  
The native DLL owns the D3D12 device, swapchain, command buffer, render pipeline, and draw call.

## Native API

The native DLL exposes a small C ABI:

- `LightWpf_Create`
- `LightWpf_Destroy`
- `LightWpf_GetChildWindow`
- `LightWpf_Resize`
- `LightWpf_Render`
- `LightWpf_SetClearColor`
- `LightWpf_SetAnimationSpeed`

That keeps C# away from C++ classes, `std::string`, exceptions, and D3D12 COM types.

## WPF Host

`LightD3D12Host` derives from `HwndHost`.

It creates a child Win32 window, passes that HWND to the native DLL, and renders once per WPF frame using `CompositionTarget.Rendering`.

## Build Order

Build the native DLL first:

```powershell
MSBuild bindings\LightD3D12WpfNative\LightD3D12WpfNative.vcxproj /p:Configuration=Debug /p:Platform=x64
```

Then build or run the WPF app:

```powershell
dotnet run --project samples\WpfTriangle\WpfTriangle.csproj -p:Platform=x64
```

When built from the solution, the WPF project depends on the native DLL project.
