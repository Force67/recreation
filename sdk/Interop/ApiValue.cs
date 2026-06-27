using System.Runtime.InteropServices;

namespace Recreation.Interop;

// The kind tag of an ApiValue. Mirrors host/bridge.h ApiKind exactly; append
// only, never reorder.
internal enum ApiKind : int
{
    None = 0,
    Int = 1,
    Float = 2,
    Bool = 3,
    String = 4,
    Object = 5,
    Array = 6,
}

// The on-the-wire value the engine and managed code exchange. Byte-for-byte
// mirror of host/bridge.h ApiValue; only one payload field is meaningful per
// kind. This is the raw boundary type: user code never sees it, it works with
// the ergonomic Value wrapper instead.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ApiValue
{
    public ApiKind Kind;
    public int I;     // Int, or Bool (0/1)
    public float F;   // Float
    public ulong H;   // Object handle / Array id
    public byte* S;   // String (borrowed UTF-8), valid until the next bridge call
}
