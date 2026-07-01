using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Recreation.Interop;

// The production engine backend: marshals SDK calls across the native
// ScriptBridge. It owns the boundary, turning dynamically typed Values into the
// raw ApiValue wire format and pinning UTF-8 strings for the duration of each
// call. Nothing outside this class touches the bridge pointer.
//
// Single-threaded by contract: the host drives the guest from one thread and
// each call blocks until the guest replies, so the string scratch the engine
// returns is read before the next call.
internal sealed unsafe class NativeBackend : IEngineBackend
{
    private readonly ScriptBridge* _bridge;

    public NativeBackend(ScriptBridge* bridge) => _bridge = bridge;

    public bool IsScriptLoaded(string type)
    {
        using var t = new Utf8(type);
        return _bridge->IsScriptLoaded(_bridge->Ctx, t.Ptr) != 0;
    }

    public bool LoadScript(string type)
    {
        using var t = new Utf8(type);
        return _bridge->LoadScript(_bridge->Ctx, t.Ptr) != 0;
    }

    public ulong CreateInstance(string type)
    {
        using var t = new Utf8(type);
        return _bridge->CreateInstance(_bridge->Ctx, t.Ptr);
    }

    public string TypeOf(ulong handle)
    {
        const int cap = 128;
        byte* buf = stackalloc byte[cap];
        int len = _bridge->TypeOf(_bridge->Ctx, handle, buf, cap);
        if (len <= 0) return string.Empty;
        return Marshal.PtrToStringUTF8((IntPtr)buf) ?? string.Empty;
    }

    public Value CallGlobal(string type, string function, ReadOnlySpan<Value> args)
    {
        // Marshal the whole call out of a reusable per-thread arena, so a hot
        // per-frame call allocates and frees nothing on the native heap.
        Arena.Reset(ArenaBytes(type, function, args));
        byte* t = Arena.PushUtf8(type);
        byte* f = Arena.PushUtf8(function);
        ApiValue* vals = PackArgs(args);
        ApiValue result;
        _bridge->CallGlobal(_bridge->Ctx, t, f, vals, args.Length, &result);
        return FromApi(result);
    }

    public Value CallMethod(ulong self, string function, ReadOnlySpan<Value> args)
    {
        Arena.Reset(ArenaBytes(null, function, args));
        byte* f = Arena.PushUtf8(function);
        ApiValue* vals = PackArgs(args);
        ApiValue result;
        _bridge->CallMethod(_bridge->Ctx, self, f, vals, args.Length, &result);
        return FromApi(result);
    }

    public Value GetProperty(ulong self, string name)
    {
        Arena.Reset(Arena.Utf8Bound(name));
        byte* n = Arena.PushUtf8(name);
        ApiValue result;
        _bridge->GetProperty(_bridge->Ctx, self, n, &result);
        return FromApi(result);
    }

    public void SetProperty(ulong self, string name, Value value)
    {
        string? s = value.Kind == ValueKind.String ? value.AsString() : null;
        Arena.Reset(Arena.Utf8Bound(name) + Arena.Utf8Bound(s));
        byte* n = Arena.PushUtf8(name);
        byte* sp = s == null ? null : Arena.PushUtf8(s);
        _bridge->SetProperty(_bridge->Ctx, self, n, ToApi(value, sp));
    }

    // Upper bound on the arena bytes a call needs: the type + function names, the
    // ApiValue array, and UTF-8 for every string argument. Reserving once up front
    // means PushUtf8/PackArgs never reallocate mid-call (which would dangle the
    // pointers already handed to the bridge).
    private static int ArenaBytes(string? type, string function, ReadOnlySpan<Value> args)
    {
        // +8 covers the alignment padding the ApiValue-array push may consume
        // (string pushes carry their own slack in Utf8Bound).
        int bytes = Arena.Utf8Bound(type) + Arena.Utf8Bound(function)
                    + Marshalling.Align8(args.Length * sizeof(ApiValue)) + 8;
        for (int i = 0; i < args.Length; i++)
            if (args[i].Kind == ValueKind.String) bytes += Arena.Utf8Bound(args[i].AsString());
        return bytes;
    }

    // Writes the argument span into a native ApiValue array carved from the arena,
    // marshalling any string arguments into the same arena. No per-call malloc.
    private static ApiValue* PackArgs(ReadOnlySpan<Value> args)
    {
        var vals = (ApiValue*)Arena.Push(Marshalling.Align8(args.Length * sizeof(ApiValue)));
        for (int i = 0; i < args.Length; i++)
        {
            byte* sp = args[i].Kind == ValueKind.String ? Arena.PushUtf8(args[i].AsString()) : null;
            vals[i] = ToApi(args[i], sp);
        }
        return vals;
    }

    public void Tick(float deltaTime) => _bridge->Tick(_bridge->Ctx, deltaTime);

    // --- marshalling ----------------------------------------------------------

    private static Value FromApi(in ApiValue v) => v.Kind switch
    {
        ApiKind.Int => Value.Int(v.I),
        ApiKind.Float => Value.Float(v.F),
        ApiKind.Bool => Value.Bool(v.I != 0),
        ApiKind.String => Value.String(v.S != null ? Marshal.PtrToStringUTF8((IntPtr)v.S) ?? "" : ""),
        ApiKind.Object => Value.Object(v.H),
        ApiKind.Array => Value.Array(v.H),
        _ => Value.None,
    };

    private static ApiValue ToApi(in Value v, byte* stringPtr)
    {
        var a = new ApiValue();
        switch (v.Kind)
        {
            case ValueKind.Int: a.Kind = ApiKind.Int; a.I = v.AsInt(); break;
            case ValueKind.Float: a.Kind = ApiKind.Float; a.F = v.AsFloat(); break;
            case ValueKind.Bool: a.Kind = ApiKind.Bool; a.I = v.AsBool() ? 1 : 0; break;
            case ValueKind.String: a.Kind = ApiKind.String; a.S = stringPtr; break;
            case ValueKind.Object: a.Kind = ApiKind.Object; a.H = v.AsHandle(); break;
            case ValueKind.Array: a.Kind = ApiKind.Array; a.H = v.AsHandle(); break;
            default: a.Kind = ApiKind.None; break;
        }
        return a;
    }

    private static class Marshalling
    {
        // Rounds up to an 8-byte boundary so an ApiValue (whose widest member is
        // 8 bytes) carved from the arena stays naturally aligned.
        public static int Align8(int n) => (n + 7) & ~7;
    }

    // A per-thread bump allocator over one grow-only native buffer. Each call
    // Reset()s it (reserving the whole call's worth of bytes up front, so the
    // subsequent Push calls never reallocate), then Push()es its ApiValue array
    // and UTF-8 strings out of it. Nothing is freed between calls: the buffer is
    // reused, so the steady-state per-call native allocation is zero. [ThreadStatic]
    // keeps it safe when the guest thread and the main thread both marshal calls
    // (each thread gets its own arena), and the boundary is non-reentrant per
    // thread (engine natives never call back into managed), so a single arena per
    // thread cannot be clobbered mid-call.
    private static class Arena
    {
        [ThreadStatic] private static byte* _buf;
        [ThreadStatic] private static nuint _cap;
        [ThreadStatic] private static nuint _used;

        // Upper bound on the bytes PushUtf8(s) consumes: worst-case 4 UTF-8 bytes
        // per char, a null terminator, and up to 7 bytes of 8-byte alignment slack.
        public static int Utf8Bound(string? s) => s == null ? 0 : s.Length * 4 + 1 + 7;

        // Begins a call: ensures at least `bytes` are available and rewinds the
        // bump pointer. Growth happens here, before any Push hands out a pointer,
        // so live pointers are never invalidated mid-call.
        public static void Reset(int bytes)
        {
            if ((nuint)bytes > _cap)
            {
                nuint next = _cap == 0 ? 256 : _cap;
                while (next < (nuint)bytes) next *= 2;
                _buf = (byte*)NativeMemory.Realloc(_buf, next);
                _cap = next;
            }
            _used = 0;
        }

        public static byte* Push(int bytes)
        {
            nuint off = (_used + 7) & ~(nuint)7;  // 8-align
            _used = off + (nuint)bytes;
            return _buf + off;
        }

        public static byte* PushUtf8(string s)
        {
            int n = Encoding.UTF8.GetByteCount(s);
            byte* p = Push(n + 1);
            Encoding.UTF8.GetBytes(s, new Span<byte>(p, n));
            p[n] = 0;
            return p;
        }
    }

    // RAII UTF-8 marshalling for a single short-lived string argument. A null
    // string yields a null pointer. Used by the cold setup calls (script load /
    // instance creation); the per-frame hot path marshals through Arena instead.
    private readonly ref struct Utf8 : IDisposable
    {
        private readonly IntPtr _ptr;
        public Utf8(string? s) => _ptr = s == null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(s);
        public byte* Ptr => (byte*)_ptr;
        public void Dispose()
        {
            if (_ptr != IntPtr.Zero) Marshal.FreeCoTaskMem(_ptr);
        }
    }
}
