using System.Runtime.InteropServices;

namespace Recreation.Interop;

// Byte-for-byte mirror of rec::ugui_cs::WidgetOps (runtime/ugui_csharp_host.h):
// the table the native ultragui backend exposes so managed UI handlers can read
// and mutate live widgets. A widget is a packed ugui WidgetId (generation << 32 |
// index); 0 is the null widget. Append-only, never reorder.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct WidgetOps
{
    public delegate* unmanaged<byte*, ulong> Find;

    public delegate* unmanaged<ulong, byte*, int, int> GetText;  // -> full length
    public delegate* unmanaged<ulong, byte*, void> SetText;

    public delegate* unmanaged<ulong, int> GetChecked;
    public delegate* unmanaged<ulong, int, void> SetChecked;

    public delegate* unmanaged<ulong, float> GetValue;
    public delegate* unmanaged<ulong, float, void> SetValue;

    public delegate* unmanaged<ulong, int> GetSelected;
    public delegate* unmanaged<ulong, int, void> SetSelected;

    public delegate* unmanaged<ulong, int> GetVisible;
    public delegate* unmanaged<ulong, int, void> SetVisible;
}
