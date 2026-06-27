using System;

namespace Recreation.Interop;

// The seam between the SDK's Rpc surface and whatever carries multiplayer
// traffic. The production backend marshals across the native RpcBridge; tests
// supply a fake that records the calls, so mod RPC logic runs without a live
// network. Rpc funnels every outbound send and subscription through here.
internal interface IRpcBackend
{
    void Emit(RpcTarget target, uint peer, string name, ReadOnlySpan<Value> args);
    void Subscribe(string name);
}
