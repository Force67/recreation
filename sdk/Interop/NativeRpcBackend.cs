using System;
using System.Runtime.InteropServices;

namespace Recreation.Interop;

// The production RPC backend: marshals Rpc sends and subscriptions across the
// native RpcBridge. It turns Values into the raw ApiValue wire format and pins
// UTF-8 for the duration of each call, mirroring NativeBackend's boundary work.
//
// Single-threaded by contract: the host drives the guest from one thread, so the
// pinned string scratch lives only across the one blocking call that reads it.
internal sealed unsafe class NativeRpcBackend : IRpcBackend
{
    // Copied by value: a struct of pointers the engine keeps alive for the
    // process, so holding it without pinning is safe.
    private readonly RpcBridge _bridge;

    public NativeRpcBackend(RpcBridge bridge) => _bridge = bridge;

    public void Emit(RpcTarget target, uint peer, string name, ReadOnlySpan<Value> args)
    {
        if (_bridge.Emit == null) return;
        IntPtr namePtr = Marshal.StringToCoTaskMemUTF8(name);
        var packed = new ArgPack(args);
        try
        {
            _bridge.Emit(_bridge.Ctx, (int)target, peer, (byte*)namePtr, packed.Values, args.Length);
        }
        finally
        {
            packed.Free();
            Marshal.FreeCoTaskMem(namePtr);
        }
    }

    public void Subscribe(string name)
    {
        if (_bridge.On == null) return;
        IntPtr namePtr = Marshal.StringToCoTaskMemUTF8(name);
        try { _bridge.On(_bridge.Ctx, (byte*)namePtr); }
        finally { Marshal.FreeCoTaskMem(namePtr); }
    }

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
    // emit has returned. Same approach as NativeBackend.ArgPack, replicated here
    // because that type is private to NativeBackend.
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
}
