#include "engine.h"

// Bridges the managed (C#) scripting world to the multiplayer RPC channel. The
// managed host boots before the session exists, so a subscription only records a
// name here; StartNetworking later registers a forwarding handler on the live
// session's RPC registry. Outbound emits route to whichever session role is
// active. Compiles away without networking.
#if RECREATION_HAS_NET

#include <cstdint>
#include <string>
#include <vector>

#include "net/rpc_channel.h"
#include "gamenet/session.h"
#include "rpc/rpc_message.h"
#include "rpc/rpc_value.h"
#include "script/host/bridge.h"
#include "script/host/managed_host.h"

namespace rx {
namespace {

using script::host::ApiKind;
using script::host::ApiValue;

// Marshals a managed argument into an RPC value. Strings are copied; object
// handles travel as integers, so a script can pass form handles peer to peer.
rpc::RpcValue FromApi(const ApiValue& a) {
  switch (a.kind) {
    case ApiKind::kInt:
      return rpc::RpcValue(static_cast<i64>(a.i));
    case ApiKind::kFloat:
      return rpc::RpcValue(static_cast<f64>(a.f));
    case ApiKind::kBool:
      return rpc::RpcValue(a.i != 0);
    case ApiKind::kString:
      return rpc::RpcValue(std::string(a.s ? a.s : ""));
    case ApiKind::kObject:
      return rpc::RpcValue(static_cast<i64>(a.h));
    default:  // kNone, kArray do not cross the RPC boundary
      return rpc::RpcValue();
  }
}

// Marshals an RPC value back into a managed argument. The string view borrows the
// RpcValue's storage, valid for the duration of the dispatch call.
ApiValue ToApi(const rpc::RpcValue& v) {
  ApiValue a;
  switch (v.type()) {
    case rpc::RpcValue::Type::kBool:
      a.kind = ApiKind::kBool;
      a.i = v.as_bool() ? 1 : 0;
      break;
    case rpc::RpcValue::Type::kInt: {
      // Form/object handles are 64-bit, so a value that does not fit the 32-bit
      // int payload travels in the handle field instead of truncating; managed
      // code reads it back as an object handle, which is what such values are.
      const i64 n = v.as_int();
      if (n >= INT32_MIN && n <= INT32_MAX) {
        a.kind = ApiKind::kInt;
        a.i = static_cast<std::int32_t>(n);
      } else {
        a.kind = ApiKind::kObject;
        a.h = static_cast<std::uint64_t>(n);
      }
      break;
    }
    case rpc::RpcValue::Type::kFloat:
      a.kind = ApiKind::kFloat;
      a.f = static_cast<float>(v.as_float());
      break;
    case rpc::RpcValue::Type::kString:
      a.kind = ApiKind::kString;
      a.s = v.as_string().c_str();
      break;
    default:  // kNull, kBlob
      a.kind = ApiKind::kNone;
      break;
  }
  return a;
}

// Registers a forwarding handler so an inbound call of `name` reaches managed
// code. Pure of Engine internals (takes the registry and host directly), so it
// can live outside the friend functions.
void RegisterForwardingOn(rpc::RpcRegistry* registry, script::host::ManagedHost* managed,
                          std::string name) {
  if (!registry || !managed) return;
  registry->On(name, [managed, name](const rpc::RpcContext& ctx, const rpc::RpcArgs& args) {
    std::vector<ApiValue> api;
    api.reserve(args.size());
    for (const rpc::RpcValue& v : args) api.push_back(ToApi(v));
    managed->DispatchRpc(name.c_str(), static_cast<std::int32_t>(ctx.sender),
                         ctx.from_server ? 1 : 0, api.data(),
                         static_cast<std::int32_t>(api.size()));
  });
}

// The active session's RPC registry, server role preferred, or null in single
// player / before the session opens.
rpc::RpcRegistry* ActiveRegistry(net::GameServerSession* server, net::GameClientSession* client) {
  if (server && server->rpc()) return &server->rpc()->registry();
  if (client && client->rpc()) return &client->rpc()->registry();
  return nullptr;
}

}  // namespace

// Sends a managed RPC over the live session. A target the current role cannot
// satisfy (a broadcast with no server, say) silently drops.
void EngineRpcEmitImpl(Engine& e, std::int32_t target, std::uint64_t peer, const char* name,
                       const ApiValue* args, std::int32_t argc) {
  rpc::RpcCall call;
  call.name = name ? name : "";
  if (argc > 0) call.args.reserve(static_cast<size_t>(argc));
  for (std::int32_t i = 0; i < argc; ++i) call.args.push_back(FromApi(args[i]));

  switch (static_cast<script::host::RpcTarget>(target)) {
    case script::host::RpcTarget::kToServer:
      if (e.client_session_ && e.client_session_->rpc()) e.client_session_->rpc()->EmitToServer(call);
      break;
    case script::host::RpcTarget::kToClient:
      if (e.server_session_ && e.server_session_->rpc())
        e.server_session_->rpc()->EmitToClient(static_cast<u32>(peer), call);
      break;
    case script::host::RpcTarget::kBroadcast:
      if (e.server_session_ && e.server_session_->rpc()) e.server_session_->rpc()->Broadcast(call);
      break;
  }
}

// Records a managed subscription. Forwards from the session immediately when one
// is already live; otherwise StartNetworking forwards it once the session opens.
void EngineRpcSubscribeImpl(Engine& e, const char* name) {
  if (!name || !*name) return;
  std::string n(name);
  for (const std::string& existing : e.managed_rpc_names_) {
    if (existing == n) return;  // already subscribed
  }
  e.managed_rpc_names_.push_back(n);
  RegisterForwardingOn(ActiveRegistry(e.server_session_, e.client_session_), e.managed_.get(), n);
}

namespace {

void RpcEmitThunk(void* ctx, std::int32_t target, std::uint64_t peer, const char* name,
                  const ApiValue* args, std::int32_t argc) {
  EngineRpcEmitImpl(*static_cast<Engine*>(ctx), target, peer, name, args, argc);
}

void RpcOnThunk(void* ctx, const char* name) {
  EngineRpcSubscribeImpl(*static_cast<Engine*>(ctx), name);
}

}  // namespace

script::host::RpcBridge MakeManagedRpcBridge(Engine& engine) {
  return script::host::RpcBridge{&engine, &RpcEmitThunk, &RpcOnThunk};
}

void RegisterManagedRpcForwarding(Engine& e) {
  rpc::RpcRegistry* registry = ActiveRegistry(e.server_session_, e.client_session_);
  if (!registry) return;
  for (const std::string& name : e.managed_rpc_names_) {
    RegisterForwardingOn(registry, e.managed_.get(), name);
  }
}

}  // namespace rx

#endif  // RECREATION_HAS_NET
