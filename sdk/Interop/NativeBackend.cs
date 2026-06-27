using System;
using System.Runtime.InteropServices;

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
        using var t = new Utf8(type);
        using var f = new Utf8(function);
        var packed = new ArgPack(args);
        try
        {
            ApiValue result;
            _bridge->CallGlobal(_bridge->Ctx, t.Ptr, f.Ptr, packed.Values, args.Length, &result);
            return FromApi(result);
        }
        finally
        {
            packed.Free();
        }
    }

    public Value CallMethod(ulong self, string function, ReadOnlySpan<Value> args)
    {
        using var f = new Utf8(function);
        var packed = new ArgPack(args);
        try
        {
            ApiValue result;
            _bridge->CallMethod(_bridge->Ctx, self, f.Ptr, packed.Values, args.Length, &result);
            return FromApi(result);
        }
        finally
        {
            packed.Free();
        }
    }

    public Value GetProperty(ulong self, string name)
    {
        using var n = new Utf8(name);
        ApiValue result;
        _bridge->GetProperty(_bridge->Ctx, self, n.Ptr, &result);
        return FromApi(result);
    }

    public void SetProperty(ulong self, string name, Value value)
    {
        using var n = new Utf8(name);
        using var s = new Utf8(value.Kind == ValueKind.String ? value.AsString() : null);
        _bridge->SetProperty(_bridge->Ctx, self, n.Ptr, ToApi(value, s.Ptr));
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

    // Packs a span of Values into a native ApiValue array, allocating native
    // UTF-8 for any string arguments. Free() releases those allocations once the
    // call has returned.
    private readonly ref struct ArgPack
    {
        public readonly ApiValue* Values;
        private readonly IntPtr* _strings;  // native UTF-8 to free, parallel to Values
        private readonly int _count;

        public ArgPack(ReadOnlySpan<Value> args)
        {
            _count = args.Length;
            Values = (ApiValue*)NativeMemory.Alloc((nuint)(sizeof(ApiValue) * Math.Max(1, _count)));
            _strings = (IntPtr*)NativeMemory.Alloc((nuint)(sizeof(IntPtr) * Math.Max(1, _count)));
            for (int i = 0; i < _count; i++)
            {
                byte* sp = null;
                if (args[i].Kind == ValueKind.String)
                {
                    _strings[i] = Marshal.StringToCoTaskMemUTF8(args[i].AsString());
                    sp = (byte*)_strings[i];
                }
                else
                {
                    _strings[i] = IntPtr.Zero;
                }
                Values[i] = ToApi(args[i], sp);
            }
        }

        public void Free()
        {
            for (int i = 0; i < _count; i++)
                if (_strings[i] != IntPtr.Zero) Marshal.FreeCoTaskMem(_strings[i]);
            NativeMemory.Free(Values);
            NativeMemory.Free(_strings);
        }
    }

    // RAII UTF-8 marshalling for a single short-lived string argument. A null
    // string yields a null pointer.
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
