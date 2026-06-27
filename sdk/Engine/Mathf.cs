using System;

namespace Recreation;

// Scalar math helpers in the Unity idiom, so mod code interpolating values or
// clamping ranges reads the way modders expect. Thin wrappers over MathF with
// the conveniences (Clamp01, Lerp, MoveTowards, ...) the standard library lacks.
public static class Mathf
{
    public const float Deg2Rad = MathF.PI / 180f;
    public const float Rad2Deg = 180f / MathF.PI;
    public const float Epsilon = 1e-6f;

    public static float Clamp(float value, float min, float max) =>
        value < min ? min : value > max ? max : value;

    public static float Clamp01(float value) => Clamp(value, 0f, 1f);

    // Linear interpolation with t clamped to [0, 1].
    public static float Lerp(float a, float b, float t) => a + (b - a) * Clamp01(t);

    // Linear interpolation without clamping t.
    public static float LerpUnclamped(float a, float b, float t) => a + (b - a) * t;

    // The t in [0, 1] that Lerp(a, b, t) would produce `value`; 0 if a == b.
    public static float InverseLerp(float a, float b, float value) =>
        MathF.Abs(b - a) < Epsilon ? 0f : Clamp01((value - a) / (b - a));

    // Moves `current` toward `target` by at most `maxDelta`.
    public static float MoveTowards(float current, float target, float maxDelta)
    {
        if (MathF.Abs(target - current) <= maxDelta) return target;
        return current + MathF.Sign(target - current) * maxDelta;
    }

    // Loops `t` so it stays within [0, length).
    public static float Repeat(float t, float length) =>
        Clamp(t - MathF.Floor(t / length) * length, 0f, length);

    // Ping-pongs `t` back and forth in [0, length].
    public static float PingPong(float t, float length)
    {
        t = Repeat(t, length * 2f);
        return length - MathF.Abs(t - length);
    }

    public static bool Approximately(float a, float b) =>
        MathF.Abs(b - a) < MathF.Max(Epsilon * MathF.Max(MathF.Abs(a), MathF.Abs(b)), Epsilon * 8f);
}
