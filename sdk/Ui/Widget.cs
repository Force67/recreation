using System;
using System.Runtime.InteropServices;
using Recreation.Interop;

namespace Recreation;

// A live ultragui widget, addressed by its packed handle. A handler receives one
// for the widget it fired on; Ui.Find(name) resolves any registered widget. The
// handle is stable but generation-checked native-side, so a stale Widget (after a
// UI rebuild) safely reads back defaults and ignores writes.
//
// Reads and writes go straight through the native WidgetOps table the engine
// handed the managed world; there is no managed-side cache. Property semantics
// match the widget kind (Text on text/buttons, Checked on checkboxes, Value on
// sliders, Selected on dropdowns); off-kind access is a harmless no-op.
public readonly unsafe struct Widget
{
    public ulong Handle { get; }

    internal Widget(ulong handle) => Handle = handle;

    // True if this refers to a widget (not the null handle) and a UI backend is
    // bound. False handles read as defaults and swallow writes.
    public bool Valid => Handle != 0 && Ui.HasOps;

    public string Text
    {
        get
        {
            if (!Valid) return string.Empty;
            WidgetOps* ops = Ui.Ops;
            int len = ops->GetText(Handle, null, 0);
            if (len <= 0) return string.Empty;
            byte* buf = stackalloc byte[256];
            byte* dst = buf;
            IntPtr heap = IntPtr.Zero;
            if (len + 1 > 256)
            {
                heap = Marshal.AllocCoTaskMem(len + 1);
                dst = (byte*)heap;
            }
            try
            {
                ops->GetText(Handle, dst, len + 1);
                return Marshal.PtrToStringUTF8((IntPtr)dst) ?? string.Empty;
            }
            finally
            {
                if (heap != IntPtr.Zero) Marshal.FreeCoTaskMem(heap);
            }
        }
        set
        {
            if (!Valid) return;
            IntPtr s = Marshal.StringToCoTaskMemUTF8(value ?? string.Empty);
            try { Ui.Ops->SetText(Handle, (byte*)s); }
            finally { Marshal.FreeCoTaskMem(s); }
        }
    }

    public bool Checked
    {
        get => Valid && Ui.Ops->GetChecked(Handle) != 0;
        set { if (Valid) Ui.Ops->SetChecked(Handle, value ? 1 : 0); }
    }

    public float Value
    {
        get => Valid ? Ui.Ops->GetValue(Handle) : 0f;
        set { if (Valid) Ui.Ops->SetValue(Handle, value); }
    }

    public int Selected
    {
        get => Valid ? Ui.Ops->GetSelected(Handle) : -1;
        set { if (Valid) Ui.Ops->SetSelected(Handle, value); }
    }

    public bool Visible
    {
        get => Valid && Ui.Ops->GetVisible(Handle) != 0;
        set { if (Valid) Ui.Ops->SetVisible(Handle, value ? 1 : 0); }
    }
}
