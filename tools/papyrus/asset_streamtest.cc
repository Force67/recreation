// asset_streamtest: end-to-end loopback check of FiveM-style asset streaming and
// the scripting RPC channel over a real zetanet session. A server catalogs a
// temp mods directory and a client connects over localhost UDP, downloads the
// content it is missing into a content cache, and mounts it through the asset
// Vfs. It also round-trips an RPC: client to server and back. This exercises the
// threaded transport, the worker-thread sender, and the control-channel receive
// loop, so it is built (and gated) only with networking.
//
// zetanet's headers inject global arch_types scalar aliases, so rx:: scalar and
// namespace symbols are fully qualified here to avoid the ambiguity, matching the
// other net/bethesda tests.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "asset/vfs.h"
#include "core/types.h"
#include "ecs/world.h"
#include "modstream/content_provider.h"
#include "modstream/content_store.h"
#include "modstream/mod_catalog.h"
#include "gamenet/asset_stream.h"
#include "net/rpc_channel.h"
#include "gamenet/session.h"
#include "rpc/rpc_value.h"

namespace fs = std::filesystem;
namespace net = rx::net;
namespace modstream = rx::modstream;
namespace rpc = rx::rpc;
namespace ecs = rx::ecs;
namespace asset = rx::asset;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void WriteFile(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

// Pumps both sessions one fixed step and yields briefly so the threaded
// transport makes progress.
void Pump(net::GameServerSession& server, ecs::World& sworld,
          net::GameClientSession& client, ecs::World& cworld) {
  const float dt = 1.0f / 60.0f;
  server.Tick(sworld, dt);
  client.Tick(cworld, dt);
  std::this_thread::sleep_for(std::chrono::milliseconds(4));
}

}  // namespace

int main() {
  std::printf("asset_streamtest\n");

  const fs::path tmp = fs::temp_directory_path() / "rec_asset_stream_test";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  const fs::path mods_dir = tmp / "server_mods";
  const fs::path cache_dir = tmp / "client_cache";

  WriteFile(mods_dir / "weapons" / "meshes" / "sword.nif", "MESH" + std::string(3000, 'm'));
  WriteFile(mods_dir / "weapons" / "textures" / "sword.dds", "TEX" + std::string(9000, 't'));
  WriteFile(mods_dir / "scripts" / "main.lua", "print('hi')");

  std::optional<modstream::ModCatalog> catalog = modstream::ModCatalog::Build(mods_dir);
  Check("server catalogs the mods dir", catalog.has_value());
  if (!catalog) {
    std::printf("asset_streamtest: %d failure(s)\n", g_failures + 1);
    return 1;
  }
  modstream::ContentStore store(cache_dir);
  // Declared before the server so it outlives the catalog the server points at
  // after a live reload below.
  std::optional<modstream::ModCatalog> reload_catalog;

  // --- bring up a loopback server and client ---
  net::GameSessionConfig server_cfg;
  server_cfg.port = 29751;
  server_cfg.mod_catalog = &*catalog;
  server_cfg.client_timeout_seconds = 1.0f;  // so the leave hook is testable quickly
  net::GameServerSession server(server_cfg);
  Check("server starts", server.Start());

  net::GameSessionConfig client_cfg;
  client_cfg.port = 29751;
  client_cfg.address = base::String("127.0.0.1");
  client_cfg.content_store = &store;
  net::GameClientSession client(client_cfg);
  Check("client starts", client.Start());

  // Register RPC handlers before any traffic flows. The server echoes back to
  // the sender; the client records the reply.
  bool server_saw_rpc = false;
  rx::i64 echoed_value = 0;
  bool client_saw_reply = false;
  server.rpc()->registry().On("echo", [&](const rpc::RpcContext& ctx, const rpc::RpcArgs& args) {
    server_saw_rpc = true;
    const rx::i64 v = args.empty() ? 0 : args[0].as_int();
    server.rpc()->EmitToClient(ctx.sender, "echo_reply", {rpc::RpcValue(rx::i64{v + 1})});
  });
  client.rpc()->registry().On("echo_reply", [&](const rpc::RpcContext&, const rpc::RpcArgs& args) {
    client_saw_reply = true;
    if (!args.empty()) echoed_value = args[0].as_int();
  });

  // The server learns when the client finished streaming, the hook server-side
  // scripts use to gate spawn or greet the player.
  bool server_saw_ready = false;
  rx::u32 ready_peer = 9999;
  server.SetClientReadySink([&](rx::u32 peer) {
    server_saw_ready = true;
    ready_peer = peer;
  });

  // The fundamental multiplayer lifecycle hooks.
  bool server_saw_join = false;
  rx::u32 join_peer = 9999;
  server.SetClientJoinedSink([&](rx::u32 peer) {
    server_saw_join = true;
    join_peer = peer;
  });
  bool server_saw_left = false;
  rx::u32 left_peer = 9999;
  server.SetClientLeftSink([&](rx::u32 peer) {
    server_saw_left = true;
    left_peer = peer;
  });

  // Mount each ready manifest into a persistent Vfs (re-fired on a live reload),
  // exactly as the engine does, so the test can read streamed content back.
  asset::Vfs client_vfs;
  int mount_count = 0;
  client.asset_stream()->set_on_ready([&](const modstream::ModManifest& manifest) {
    client_vfs.UnmountByPrefix("modstream:");
    modstream::MountManifest(client_vfs, manifest, store);
    ++mount_count;
  });

  ecs::World sworld;
  ecs::World cworld;

  // Stream the mods. Generous bound: localhost should finish in well under a
  // second, but the handshake plus reliable retransmit timing varies.
  bool ready = false;
  for (int i = 0; i < 2000 && !ready; ++i) {
    Pump(server, sworld, client, cworld);
    if (client.asset_stream()->failed()) break;
    ready = client.asset_stream()->ready();
  }
  Check("client joined the session", client.joined());
  Check("server raised the join hook", server_saw_join);
  Check("join hook carries the peer id", join_peer == 0);
  Check("asset stream did not fail", !client.asset_stream()->failed());
  Check("all mod content streamed and cached", ready);
  Check("streamed mods mounted once", mount_count == 1);
  Check("streamed mesh reads back through the Vfs", [&] {
    auto mesh = client_vfs.Read("meshes/sword.nif");
    return mesh && mesh->size() == 3004;
  }());
  Check("streamed script reads back through the Vfs",
        client_vfs.Read("main.lua").has_value());

  // Every catalogued file now lives in the cache, keyed by content hash.
  bool all_cached = true;
  for (const modstream::ModResource& resource : catalog->manifest().resources) {
    for (const modstream::ResourceFile& file : resource.files) {
      if (!store.Has(file.hash)) all_cached = false;
    }
  }
  Check("content cache holds every catalogued file", all_cached);
  Check("client reports no files remaining", client.asset_stream()->files_remaining() == 0);

  // The ready notice rides the wire after the client mounts; settle a few ticks.
  for (int i = 0; i < 120 && !server_saw_ready; ++i) Pump(server, sworld, client, cworld);
  Check("server was notified the client's assets are ready", server_saw_ready);
  Check("ready notice carries the client's peer id", ready_peer == 0);

  // --- RPC round-trip ---
  client.rpc()->EmitToServer("echo", {rpc::RpcValue(rx::i64{41})});
  for (int i = 0; i < 600 && !client_saw_reply; ++i) Pump(server, sworld, client, cworld);
  Check("server received the client's RPC", server_saw_rpc);
  Check("client received the server's reply", client_saw_reply);
  Check("RPC argument round-tripped", echoed_value == 42);

  // --- live mod reload pushed to the connected client ---
  WriteFile(mods_dir / "scripts" / "extra.lua", "print('live update')");
  reload_catalog = modstream::ModCatalog::Build(mods_dir);
  Check("server re-catalogs after a mod edit", reload_catalog.has_value());
  if (reload_catalog) {
    const int mounts_before = mount_count;
    server.ReloadCatalog(*reload_catalog);  // bumps the generation + re-sends
    for (int i = 0; i < 1500 && mount_count == mounts_before; ++i)
      Pump(server, sworld, client, cworld);
    Check("connected client re-mounted after the live reload",
          mount_count == mounts_before + 1);
    Check("the live-added file resolves on the client",
          client_vfs.Read("extra.lua").has_value());
    Check("unchanged content still resolves after the reload",
          client_vfs.Read("meshes/sword.nif").has_value());
  }

  // Stop ticking the client and run the server alone until it times the peer out,
  // exercising the leave hook.
  const float dt = 1.0f / 60.0f;
  for (int i = 0; i < 200 && !server_saw_left; ++i) {
    server.Tick(sworld, dt);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Check("server raised the leave hook on timeout", server_saw_left);
  Check("leave hook carries the peer id", left_peer == 0);

  fs::remove_all(tmp, ec);
  std::printf("asset_streamtest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
