using System;

namespace Recreation;

// A 3D vector in engine world space. A small, Unity-flavoured value type for the
// spatial parts of the API (positions, offsets, distances). Immutable; the
// arithmetic operators return new vectors.
public readonly struct Vector3 : IEquatable<Vector3>
{
    public float X { get; }
    public float Y { get; }
    public float Z { get; }

    public Vector3(float x, float y, float z)
    {
        X = x;
        Y = y;
        Z = z;
    }

    public static readonly Vector3 Zero = new(0, 0, 0);
    public static readonly Vector3 One = new(1, 1, 1);

    public float LengthSquared => X * X + Y * Y + Z * Z;
    public float Length => MathF.Sqrt(LengthSquared);

    // The unit vector in this direction, or Zero for a zero-length vector.
    public Vector3 Normalized
    {
        get
        {
            float len = Length;
            return len > Mathf.Epsilon ? this * (1f / len) : Zero;
        }
    }

    public static Vector3 operator +(Vector3 a, Vector3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vector3 operator -(Vector3 a, Vector3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Vector3 operator *(Vector3 a, float s) => new(a.X * s, a.Y * s, a.Z * s);
    public static Vector3 operator -(Vector3 a) => new(-a.X, -a.Y, -a.Z);

    public static float Distance(Vector3 a, Vector3 b) => (a - b).Length;
    public static float Dot(Vector3 a, Vector3 b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z;

    public static Vector3 Cross(Vector3 a, Vector3 b) =>
        new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);

    // Component-wise linear interpolation, t clamped to [0, 1].
    public static Vector3 Lerp(Vector3 a, Vector3 b, float t)
    {
        t = Mathf.Clamp01(t);
        return new Vector3(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t);
    }

    // Moves `current` toward `target` by at most `maxDelta`.
    public static Vector3 MoveTowards(Vector3 current, Vector3 target, float maxDelta)
    {
        Vector3 delta = target - current;
        float dist = delta.Length;
        if (dist <= maxDelta || dist < Mathf.Epsilon) return target;
        return current + delta * (maxDelta / dist);
    }

    public bool Equals(Vector3 other) => X == other.X && Y == other.Y && Z == other.Z;
    public override bool Equals(object? obj) => obj is Vector3 v && Equals(v);
    public override int GetHashCode() => HashCode.Combine(X, Y, Z);
    public override string ToString() => $"({X}, {Y}, {Z})";
}
