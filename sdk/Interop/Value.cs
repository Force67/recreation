using System;

namespace Recreation.Interop;

// The kind of a dynamically typed engine value. Mirrors the Papyrus value
// kinds that cross the boundary.
public enum ValueKind
{
    None,
    Int,
    Float,
    Bool,
    String,
    Object,
    Array,
}

// A dynamically typed value exchanged with the engine. The managed analog of a
// Papyrus value: it owns its payload (strings are copied out of native memory),
// so it stays valid after the originating call returns. Construct through the
// implicit conversions; read through the As* accessors or the implicit casts.
//
// This is the ergonomic boundary type. Engine wrappers (Form, Actor, ...) build
// these to pass arguments and unpack them from results, so user code rarely
// touches Value directly.
public readonly struct Value
{
    public ValueKind Kind { get; }

    private readonly long _scalar;     // Int/Bool bits, or the object/array handle
    private readonly float _float;
    private readonly string? _string;

    private Value(ValueKind kind, long scalar, float f, string? s)
    {
        Kind = kind;
        _scalar = scalar;
        _float = f;
        _string = s;
    }

    public static readonly Value None = new(ValueKind.None, 0, 0, null);

    public static Value Int(int v) => new(ValueKind.Int, v, 0, null);
    public static Value Float(float v) => new(ValueKind.Float, 0, v, null);
    public static Value Bool(bool v) => new(ValueKind.Bool, v ? 1 : 0, 0, null);
    public static Value String(string v) => new(ValueKind.String, 0, 0, v);
    public static Value Object(ulong handle) => new(ValueKind.Object, (long)handle, 0, null);
    public static Value Array(ulong id) => new(ValueKind.Array, (long)id, 0, null);

    public bool IsNone => Kind == ValueKind.None;

    public int AsInt() => Kind switch
    {
        ValueKind.Int => (int)_scalar,
        ValueKind.Bool => (int)_scalar,
        ValueKind.Float => (int)_float,
        _ => 0,
    };

    public float AsFloat() => Kind switch
    {
        ValueKind.Float => _float,
        ValueKind.Int => _scalar,
        ValueKind.Bool => _scalar,
        _ => 0f,
    };

    public bool AsBool() => Kind switch
    {
        ValueKind.Bool => _scalar != 0,
        ValueKind.Int => _scalar != 0,
        ValueKind.Float => _float != 0f,
        ValueKind.Object => _scalar != 0,
        ValueKind.String => !string.IsNullOrEmpty(_string),
        _ => false,
    };

    public string AsString() => _string ?? string.Empty;
    public ulong AsHandle() => (ulong)_scalar;

    public static implicit operator Value(int v) => Int(v);
    public static implicit operator Value(float v) => Float(v);
    public static implicit operator Value(bool v) => Bool(v);
    public static implicit operator Value(string v) => String(v);

    public override string ToString() => Kind switch
    {
        ValueKind.None => "None",
        ValueKind.Int => _scalar.ToString(),
        ValueKind.Float => _float.ToString(),
        ValueKind.Bool => _scalar != 0 ? "True" : "False",
        ValueKind.String => _string ?? string.Empty,
        ValueKind.Object => $"Object(0x{_scalar:X})",
        ValueKind.Array => $"Array({_scalar})",
        _ => "?",
    };
}
