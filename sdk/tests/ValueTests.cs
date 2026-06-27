using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers the dynamically typed Value and the Vector3 math type, the marshalling
// building blocks the whole SDK rests on.
public static class ValueTests
{
    public static void Run(Check check)
    {
        check.Equal("int round-trips", 42, Value.Int(42).AsInt());
        check.Equal("float round-trips", 1.5f, Value.Float(1.5f).AsFloat());
        check.That("bool round-trips", Value.Bool(true).AsBool());
        check.Equal("string round-trips", "hi", Value.String("hi").AsString());
        check.Equal("object handle round-trips", 0x14UL, Value.Object(0x14).AsHandle());

        check.Equal("int coerces to float", 3f, Value.Int(3).AsFloat());
        check.That("nonzero int is truthy", Value.Int(1).AsBool());
        check.That("none is none", Value.None.IsNone);

        Value implicitInt = 7;
        check.Equal("implicit int conversion", 7, implicitInt.AsInt());
        Value implicitStr = "x";
        check.Equal("implicit string conversion", ValueKind.String, implicitStr.Kind);

        var a = new Vector3(1, 2, 3);
        var b = new Vector3(1, 0, 3);
        check.Equal("vector add", new Vector3(2, 2, 6), a + b);
        check.Equal("vector sub", new Vector3(0, 2, 0), a - b);
        check.Equal("vector scale", new Vector3(2, 4, 6), a * 2f);
        check.Equal("vector distance", 2f, Vector3.Distance(a, b));
        check.That("vector equality", new Vector3(1, 2, 3).Equals(a));

        check.Equal("vector dot", 10f, Vector3.Dot(new Vector3(1, 0, 3), new Vector3(1, 5, 3)));
        check.Equal("vector cross", new Vector3(0, 0, 1),
                    Vector3.Cross(new Vector3(1, 0, 0), new Vector3(0, 1, 0)));
        check.Equal("unit vector length", 1f, new Vector3(0, 3, 0).Normalized.Length);
        check.Equal("zero normalizes to zero", Vector3.Zero, Vector3.Zero.Normalized);
        check.Equal("vector lerp midpoint", new Vector3(0, 5, 0),
                    Vector3.Lerp(Vector3.Zero, new Vector3(0, 10, 0), 0.5f));
        check.Equal("move towards clamps step", new Vector3(0, 2, 0),
                    Vector3.MoveTowards(Vector3.Zero, new Vector3(0, 10, 0), 2f));

        // Mathf.
        check.Equal("clamp01 high", 1f, Mathf.Clamp01(5f));
        check.Equal("lerp clamps t", 10f, Mathf.Lerp(0f, 10f, 2f));
        check.Equal("inverse lerp", 0.5f, Mathf.InverseLerp(0f, 10f, 5f));
        check.Equal("move towards", 4f, Mathf.MoveTowards(2f, 10f, 2f));
        check.Equal("repeat wraps", 1f, Mathf.Repeat(7f, 3f));
        check.Equal("pingpong", 1f, Mathf.PingPong(5f, 3f));
        check.That("approximately", Mathf.Approximately(1f, 1f + 1e-7f));
    }
}
