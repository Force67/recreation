// client_scripts_nettest: loopback check of the client-script offer. A server
// resource declares managed assemblies in client_scripts.txt; on join the
// server streams the files like any other content and then sends the
// kClientScripts offer naming which of them are client assemblies. The test
// verifies the offer survives the catalog (missing and streamignored scripts
// dropped), reaches the client validated against the manifest, resolves to
// cached files, and updates on a live reload.
//
// zetanet's headers inject global arch_types scalar aliases, so rx:: scalar and
// namespace symbols are fully qualified here, matching the other net tests.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "components/gamenet/asset_stream.h"
#include "components/gamenet/session.h"
#include "components/modstream/content_hash.h"
#include "components/modstream/content_store.h"
#include "components/modstream/mod_catalog.h"
#include "core/types.h"
#include "ecs/world.h"

namespace fs = std::filesystem;
namespace net = rx::net;
namespace modstream = rx::modstream;
namespace ecs = rx::ecs;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

void WriteFile(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

void Pump(net::GameServerSession& server,
          ecs::World& sworld,
          net::GameClientSession& client,
          ecs::World& cworld) {
  const float dt = 1.0f / 60.0f;
  server.Tick(sworld, dt);
  client.Tick(cworld, dt);
  std::this_thread::sleep_for(std::chrono::milliseconds(4));
}

}  // namespace

int main() {
  std::printf("client_scripts_nettest\n");

  const fs::path tmp = fs::temp_directory_path() / "rec_client_scripts_test";
  std::error_code ec;
  fs::remove_all(tmp.c_str(), ec);
  const fs::path mods_dir = tmp / "server_mods";
  const fs::path cache_dir = tmp / "client_cache";

  // One resource with an assembly clients should run, an ordinary asset, a
  // server-only assembly, and a declaration naming a third file that does not
  // exist. Only the first two stream; only the first is a client script.
  const std::string client_dll = "MZfake-managed-assembly-" + std::string(600, 'c');
  WriteFile(mods_dir / "gamedata" / "assemblies" / "client.dll", client_dll);
  WriteFile(mods_dir / "gamedata" / "textures" / "logo.dds", "DDS" + std::string(2000, 'd'));
  WriteFile(mods_dir / "gamedata" / ".streamignore", "server/\n");
  WriteFile(mods_dir / "gamedata" / "server" / "internal.dll", "MZserver-only");
  WriteFile(mods_dir / "gamedata" / "client_scripts.txt",
            "# what joining clients run\n"
            "assemblies/client.dll\n"
            "server/internal.dll\n"
            "assemblies/missing.dll\n");

  base::Optional<modstream::ModCatalog> catalog = modstream::ModCatalog::Build(mods_dir);
  Check("server catalogs the mods dir", catalog.has_value());
  if (!catalog) {
    std::printf("client_scripts_nettest: %d failure(s)\n", g_failures + 1);
    return 1;
  }
  const modstream::ModResource* gamedata = nullptr;
  for (const modstream::ModResource& resource : catalog->manifest().resources) {
    if (resource.name == "gamedata")
      gamedata = &resource;
  }
  Check("the resource declared exactly the servable client script",
        gamedata && gamedata->client_scripts.size() == 1 &&
            gamedata->client_scripts[0] == "assemblies/client.dll");

  modstream::ContentStore store(cache_dir);

  net::GameSessionConfig server_cfg;
  server_cfg.port = 29758;
  server_cfg.mod_catalog = &*catalog;
  net::GameServerSession server(server_cfg);
  Check("server starts", server.Start());

  net::GameSessionConfig client_cfg;
  client_cfg.port = 29758;
  client_cfg.address = base::String("127.0.0.1");
  client_cfg.content_store = &store;
  net::GameClientSession client(client_cfg);
  Check("client starts", client.Start());

  int mounts = 0;
  client.asset_stream()->set_on_ready([&](const modstream::ModManifest& manifest) {
    (void)manifest;
    ++mounts;
  });

  ecs::World sworld;
  ecs::World cworld;
  bool ready = false;
  for (int i = 0; i < 2000 && !ready; ++i) {
    Pump(server, sworld, client, cworld);
    ready = client.asset_stream()->ready();
  }
  Check("client joined and streamed", client.joined() && ready);

  // The offer arrives as its own reliable packet after the manifest (the two
  // are independent deliveries), so settle the pumps until it lands too.
  bool offered_arrived = false;
  for (int i = 0; i < 300 && !offered_arrived; ++i) {
    Pump(server, sworld, client, cworld);
    offered_arrived = client.asset_stream()->client_scripts().size() == 1;
  }
  Check("the client-script offer arrived with one entry", offered_arrived);
  const std::vector<modstream::ClientScriptEntry>& offered =
      client.asset_stream()->client_scripts();
  if (offered.size() == 1) {
    const modstream::ContentHash dll_hash =
        modstream::HashBytes(client_dll.data(), client_dll.size());
    Check("the offer names the assembly's resource path",
          offered[0].path == "gamedata/assemblies/client.dll");
    Check("the offer carries the assembly's content hash", offered[0].hash == dll_hash);
    Check("the offer carries the assembly's size", offered[0].size == client_dll.size());
    Check("the offered bytes are in the local cache", store.Has(offered[0].hash));
  }

  // A streamignored assembly must be neither streamed nor offered: compute its
  // hash from the bytes we wrote and check both facts.
  const std::string server_dll = "MZserver-only";
  const modstream::ContentHash server_hash =
      modstream::HashBytes(server_dll.data(), server_dll.size());
  Check("the server-only assembly is not servable", !catalog->PathForHash(server_hash));
  Check("the server-only assembly is not in the offer",
        offered.end() == std::find_if(offered.begin(), offered.end(),
                                      [&](const modstream::ClientScriptEntry& e) {
                                        return e.hash == server_hash;
                                      }));

  // --- live reload extends the declaration ---
  const std::string extra_dll = "MZextra-assembly-" + std::string(400, 'x');
  WriteFile(mods_dir / "gamedata" / "assemblies" / "extra.dll", extra_dll);
  WriteFile(mods_dir / "gamedata" / "client_scripts.txt",
            "# what joining clients run\nassemblies/client.dll\nassemblies/extra.dll\n");
  base::Optional<modstream::ModCatalog> reload = modstream::ModCatalog::Build(mods_dir);
  Check("server re-catalogs after the declaration edit", reload.has_value());
  if (reload) {
    server.ReloadCatalog(*reload);
    // The reload re-mounts as soon as its manifest is assembled; the new
    // assembly's bytes may still be in flight, so wait for both the offer and
    // the cache entry.
    const modstream::ContentHash extra_hash =
        modstream::HashBytes(extra_dll.data(), extra_dll.size());
    bool updated = false;
    for (int i = 0; i < 1500 && !updated; ++i) {
      Pump(server, sworld, client, cworld);
      for (const modstream::ClientScriptEntry& e : client.asset_stream()->client_scripts())
        updated |= (e.path == "gamedata/assemblies/extra.dll") && store.Has(e.hash);
    }
    Check("the reloaded offer reaches the connected client",
          client.asset_stream()->client_scripts().size() == 2);
    Check("the added assembly streamed and is cached", updated);
  }

  fs::remove_all(tmp.c_str(), ec);
  std::printf("client_scripts_nettest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
