using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace HelloWpfTriangle;

public sealed class LightD3D12Host : HwndHost
{
    private IntPtr _nativeContext;
    private IntPtr _childWindow;
    private bool _renderingSubscribed;

    public void SetClearColor(float red, float green, float blue)
    {
        if (_nativeContext == IntPtr.Zero)
        {
            return;
        }

        NativeMethods.LightWpf_SetClearColor(_nativeContext, red, green, blue);
    }

    public void SetAnimationSpeed(float speed)
    {
        if (_nativeContext == IntPtr.Zero)
        {
            return;
        }

        NativeMethods.LightWpf_SetAnimationSpeed(_nativeContext, speed);
    }

    protected override HandleRef BuildWindowCore(HandleRef hwndParent)
    {
        uint width = Math.Max((uint)Math.Round(ActualWidth), 1u);
        uint height = Math.Max((uint)Math.Round(ActualHeight), 1u);

        if (width <= 1u)
        {
            width = 960u;
        }

        if (height <= 1u)
        {
            height = 540u;
        }

        _nativeContext = NativeMethods.LightWpf_Create(hwndParent.Handle, width, height);
        if (_nativeContext == IntPtr.Zero)
        {
            throw new InvalidOperationException(NativeMethods.GetLastError());
        }

        _childWindow = NativeMethods.LightWpf_GetChildWindow(_nativeContext);
        if (_childWindow == IntPtr.Zero)
        {
            throw new InvalidOperationException("The native renderer did not return a child HWND.");
        }

        SubscribeRendering();
        return new HandleRef(this, _childWindow);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
        UnsubscribeRendering();

        if (_nativeContext != IntPtr.Zero)
        {
            NativeMethods.LightWpf_Destroy(_nativeContext);
            _nativeContext = IntPtr.Zero;
            _childWindow = IntPtr.Zero;
        }
    }

    protected override void OnWindowPositionChanged(Rect rcBoundingBox)
    {
        base.OnWindowPositionChanged(rcBoundingBox);

        if (_nativeContext == IntPtr.Zero)
        {
            return;
        }

        uint width = Math.Max((uint)Math.Round(rcBoundingBox.Width), 1u);
        uint height = Math.Max((uint)Math.Round(rcBoundingBox.Height), 1u);
        if (NativeMethods.LightWpf_Resize(_nativeContext, width, height) == 0)
        {
            Debug.WriteLine(NativeMethods.GetLastError());
        }
    }

    private void SubscribeRendering()
    {
        if (_renderingSubscribed)
        {
            return;
        }

        CompositionTarget.Rendering += OnRendering;
        _renderingSubscribed = true;
    }

    private void UnsubscribeRendering()
    {
        if (!_renderingSubscribed)
        {
            return;
        }

        CompositionTarget.Rendering -= OnRendering;
        _renderingSubscribed = false;
    }

    private void OnRendering(object? sender, EventArgs e)
    {
        if (_nativeContext == IntPtr.Zero || !IsVisible)
        {
            return;
        }

        if (NativeMethods.LightWpf_Render(_nativeContext) == 0)
        {
            Debug.WriteLine(NativeMethods.GetLastError());
        }
    }
}
