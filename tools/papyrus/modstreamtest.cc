// modstreamtest: deterministic checks for the asset-streaming module. It builds
// a synthetic mods directory in a temp folder, catalogs it, round-trips the
// manifest through the codec, fetches the "missing" content into a content store,
// mounts it back through the asset Vfs, and verifies the bytes come out intact.
// No game data and no socket are involved, so it runs in the ctest gate.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "asset/vfs.h"
#include "core/types.h"
#include "modstream/content_hash.h"
#include "modstream/content_provider.h"
#include "modstream/content_store.h"
#include "modstream/manifest_codec.h"
#include "modstream/asset_request.h"
#include "modstream/manifest_chunk.h"
#include "modstream/mod_catalog.h"
#include "modstream/stream_filter.h"
#include "modstream/transfer_plan.h"

namespace fs = std::filesystem;
using namespace rx;
using namespace rx::modstream;

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

const ResourceFile* FindFile(const ModResource& resource, const std::string& path) {
  for (const ResourceFile& f : resource.files) {
    if (f.path == path) return &f;
  }
  return nullptr;
}

const ModResource* FindResource(const ModManifest& manifest, const std::string& name) {
  for (const ModResource& r : manifest.resources) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

}  // namespace

int main() {
  std::printf("modstreamtest\n");

  const fs::path tmp = fs::temp_directory_path() / "rec_modstream_test";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  const fs::path mods_dir = tmp / "server_mods";
  const fs::path cache_dir = tmp / "client_cache";
  const fs::path stage_dir = tmp / "transfer_stage";
  fs::create_directories(stage_dir);

  // A "weapons" resource with a mesh and a texture, and an "audio" resource that
  // re-uses the exact same texture bytes (to exercise content-hash dedup).
  const std::string mesh = "MESH-bytes-0123456789";
  const std::string shared_tex = std::string("TEX") + std::string(5000, 'x');
  const std::string sound = "RIFFsound-data";
  WriteFile(mods_dir / "weapons" / "meshes" / "sword.nif", mesh);
  WriteFile(mods_dir / "weapons" / "textures" / "sword.dds", shared_tex);
  WriteFile(mods_dir / "audio" / "textures" / "icon.dds", shared_tex);
  WriteFile(mods_dir / "audio" / "sound" / "hit.wav", sound);

  // --- content hashing ---
  {
    const ContentHash a = HashBytes(mesh.data(), mesh.size());
    const ContentHash b = HashBytes(mesh.data(), mesh.size());
    Check("HashBytes is deterministic", a == b);
    Check("different bytes hash differently",
          HashBytes(sound.data(), sound.size()) != a);

    ContentHasher chunked;
    chunked.Update(mesh.data(), 4);
    chunked.Update(mesh.data() + 4, mesh.size() - 4);
    Check("chunked hash equals whole-buffer hash", chunked.value == a);

    const auto file_hash = HashFile(mods_dir / "weapons" / "meshes" / "sword.nif");
    Check("HashFile matches HashBytes", file_hash && *file_hash == a);
    Check("HashFile on a missing file fails", !HashFile(tmp / "nope.bin"));
  }

  // --- catalog ---
  std::optional<ModCatalog> catalog = ModCatalog::Build(mods_dir);
  Check("ModCatalog::Build succeeds", catalog.has_value());
  Check("ModCatalog::Build rejects a missing dir",
        !ModCatalog::Build(tmp / "does_not_exist"));
  if (!catalog) {
    std::printf("modstreamtest: %d failure(s)\n", g_failures + 1);
    return 1;
  }

  const ModManifest& manifest = catalog->manifest();
  Check("two resources catalogued", manifest.resources.size() == 2);
  // Resources are sorted by name, so "audio" precedes "weapons".
  Check("resources sorted by name",
        manifest.resources.size() == 2 && manifest.resources[0].name == "audio" &&
            manifest.resources[1].name == "weapons");

  const ModResource* weapons = FindResource(manifest, "weapons");
  Check("weapons resource present", weapons != nullptr);
  if (weapons) {
    // Files sorted by path: meshes/sword.nif before textures/sword.dds.
    Check("weapons files sorted by path",
          weapons->files.size() == 2 &&
              weapons->files[0].path == "meshes/sword.nif" &&
              weapons->files[1].path == "textures/sword.dds");
    const ResourceFile* nif = FindFile(*weapons, "meshes/sword.nif");
    Check("file size recorded", nif && nif->size == mesh.size());
    Check("file hash recorded",
          nif && nif->hash == HashBytes(mesh.data(), mesh.size()));
  }

  Check("manifest totals", manifest.TotalFiles() == 4 &&
                               manifest.TotalBytes() ==
                                   mesh.size() + 2 * shared_tex.size() + sound.size());

  const ContentHash tex_hash = HashBytes(shared_tex.data(), shared_tex.size());
  const auto tex_path = catalog->PathForHash(tex_hash);
  Check("PathForHash resolves a catalogued file", tex_path.has_value());
  Check("PathForHash rejects an unknown hash", !catalog->PathForHash(0xdeadbeef));

  // The host mounts its own catalogued mods straight from the mods dir.
  {
    asset::Vfs host_vfs;
    modstream::MountCatalog(host_vfs, *catalog);
    auto host_mesh = host_vfs.Read("meshes/sword.nif");
    Check("host mounts its catalogued mesh from disk",
          host_mesh && host_mesh->size() == mesh.size() &&
              std::equal(host_mesh->begin(), host_mesh->end(), mesh.begin()));
    Check("host mount serves a sound", host_vfs.Read("sound/hit.wav").has_value());
    Check("host mount misses an unknown path", !host_vfs.Read("meshes/nope.nif"));

    // Live reload swaps the mod providers: unmount them, and the content is gone.
    const size_t removed = host_vfs.UnmountByPrefix("modstream:");
    Check("unmount drops one provider per resource", removed == catalog->manifest().resources.size());
    Check("unmounted content no longer resolves", !host_vfs.Read("meshes/sword.nif"));
    modstream::MountCatalog(host_vfs, *catalog);  // re-mount, as a reload would
    Check("re-mounted content resolves again", host_vfs.Read("meshes/sword.nif").has_value());
  }

  // --- stream filter (selective distribution: keep server-only files off clients) ---
  {
    const StreamFilter f = StreamFilter::Parse(
        "# keep server bits off clients\nserver/\nsecrets.cfg\n*.bak\n\n");
    Check("filter excludes a directory prefix", f.Excludes("server/logic.lua"));
    Check("filter excludes an exact path", f.Excludes("secrets.cfg"));
    Check("filter excludes an extension", f.Excludes("data/save.bak"));
    Check("filter passes a normal file", !f.Excludes("meshes/sword.nif"));
    Check("filter is case/slash normalized",
          StreamFilter::Parse("Server\\\n").Excludes("server/x.txt"));
    Check("empty filter excludes nothing",
          StreamFilter::Parse("# only comments\n").empty() &&
              !StreamFilter::Parse("").Excludes("anything"));
  }

  // A resource whose .streamignore hides a server dir, a config and the ignore
  // file itself: none of those reach the catalogued manifest.
  {
    const fs::path filtered = mods_dir / "world";
    WriteFile(filtered / ".streamignore", "server/\nadmin.cfg\n");
    WriteFile(filtered / "meshes" / "rock.nif", "ROCK");
    WriteFile(filtered / "server" / "rules.lua", "secret server logic");
    WriteFile(filtered / "admin.cfg", "password=hunter2");
    std::optional<ModCatalog> c2 = ModCatalog::Build(mods_dir);
    Check("rebuild with a filtered resource succeeds", c2.has_value());
    const ModResource* world = c2 ? FindResource(c2->manifest(), "world") : nullptr;
    Check("filtered resource catalogues only the client file",
          world && world->files.size() == 1 && world->files[0].path == "meshes/rock.nif");
    if (c2) {
      Check("server-only file is not servable",
            !c2->PathForHash(HashBytes("secret server logic", 19)) &&
                !c2->PathForHash(HashBytes("password=hunter2", 16)));
    }
    fs::remove_all(filtered, ec);
  }

  // --- manifest codec round-trip and rejection ---
  std::vector<u8> encoded = EncodeManifest(manifest);
  std::optional<ModManifest> decoded = DecodeManifest(encoded);
  Check("manifest round-trips through the codec", decoded && *decoded == manifest);
  Check("codec rejects a truncated buffer",
        !DecodeManifest(encoded.data(), encoded.size() - 1));
  {
    std::vector<u8> bad = encoded;
    bad[0] ^= 0xff;  // corrupt the magic
    Check("codec rejects a bad magic", !DecodeManifest(bad));
    std::vector<u8> trailing = encoded;
    trailing.push_back(0);
    Check("codec rejects trailing bytes", !DecodeManifest(trailing));
  }

  // --- transfer plan (selective download) ---
  ContentStore store(cache_dir);
  std::vector<NeededFile> plan = ComputeMissing(*decoded, store);
  // Three unique blobs (mesh, shared texture, sound); the texture is shared, so
  // it appears once even though two resources reference it.
  Check("plan lists three unique blobs", plan.size() == 3);
  Check("plan bytes exclude the duplicate texture",
        PlannedBytes(plan) == mesh.size() + shared_tex.size() + sound.size());

  // --- asset-request codec ---
  {
    const std::vector<ContentHash> hashes{tex_hash, 0x1122334455667788ull, 0};
    const std::vector<u8> req = EncodeHashRequest(hashes);
    const auto decoded_req = DecodeHashRequest(req.data(), req.size(), 8);
    Check("hash request round-trips", decoded_req && *decoded_req == hashes);
    Check("hash request rejects an over-count cap",
          !DecodeHashRequest(req.data(), req.size(), 2));
    Check("hash request rejects a truncated body",
          !DecodeHashRequest(req.data(), req.size() - 1, 8));
    Check("hash request rejects trailing bytes", [&] {
      std::vector<u8> extra = req;
      extra.push_back(0);
      return !DecodeHashRequest(extra.data(), extra.size(), 8);
    }());
    Check("empty hash request round-trips",
          DecodeHashRequest(EncodeHashRequest({}).data(), 4, 8)->empty());
  }

  // --- manifest-chunk codec ---
  {
    // A two-chunk manifest: a full first chunk and a short last one.
    const u32 total = kManifestChunkPayload + 100;
    const u32 chunks = ManifestChunkCount(total);
    Check("chunk count splits correctly", chunks == 2);
    std::vector<u8> blob(total, 0x5a);
    const std::vector<u8> c0 = EncodeManifestChunk(7, total, chunks, 0, blob.data(), kManifestChunkPayload);
    const std::vector<u8> c1 = EncodeManifestChunk(7, total, chunks, 1, blob.data() + kManifestChunkPayload, 100);
    const auto v0 = DecodeManifestChunk(c0.data(), c0.size());
    const auto v1 = DecodeManifestChunk(c1.data(), c1.size());
    Check("first chunk decodes full payload",
          v0 && v0->generation == 7 && v0->chunk_index == 0 && v0->payload_len == kManifestChunkPayload);
    Check("last chunk decodes the remainder",
          v1 && v1->chunk_index == 1 && v1->payload_len == 100);
    Check("chunk rejects a header-only buffer", !DecodeManifestChunk(c0.data(), 15));
    Check("chunk rejects a wrong payload length",
          !DecodeManifestChunk(c0.data(), c0.size() - 1));
    Check("chunk rejects an out-of-range index", [&] {
      std::vector<u8> bad = EncodeManifestChunk(7, total, chunks, 5, blob.data(), kManifestChunkPayload);
      return !DecodeManifestChunk(bad.data(), bad.size());
    }());
    Check("chunk rejects an inconsistent count", [&] {
      std::vector<u8> bad = EncodeManifestChunk(7, total, 99, 0, blob.data(), kManifestChunkPayload);
      return !DecodeManifestChunk(bad.data(), bad.size());
    }());
    Check("chunk rejects a zero total", [&] {
      std::vector<u8> bad = EncodeManifestChunk(7, 0, 1, 0, blob.data(), 0);
      return !DecodeManifestChunk(bad.data(), bad.size());
    }());
  }

  // --- fetch each planned blob (the net layer would stream it; here we copy it
  // out of the server catalog into the store, exactly as a finished transfer) ---
  for (const NeededFile& need : plan) {
    const auto src = catalog->PathForHash(need.hash);
    Check("server can serve every planned hash", src.has_value());
    if (!src) continue;
    const fs::path staged = stage_dir / (std::to_string(need.hash) + ".part");
    fs::copy_file(*src, staged, fs::copy_options::overwrite_existing, ec);
    const auto stored = store.Adopt(need.hash, staged);
    Check("store adopts a verified transfer", stored.has_value());
  }
  Check("everything is cached now", ComputeMissing(*decoded, store).empty());

  // Adopt must reject content whose bytes do not match the promised hash.
  {
    const fs::path bogus = stage_dir / "bogus.part";
    WriteFile(bogus, "not what the hash claims");
    Check("store rejects a hash mismatch", !store.Adopt(tex_hash, bogus));
    Check("rejected transfer leaves no cache entry or temp file",
          store.Has(tex_hash) && !fs::exists(bogus));
  }
  Check("Store rejects a mismatched buffer",
        !store.Store(tex_hash, std::vector<u8>{1, 2, 3}));

  // --- mount the manifest back through the Vfs and read content out ---
  asset::Vfs vfs;
  MountManifest(vfs, *decoded, store);
  // Paths the Vfs resolves are resource-relative, just like loose mod files.
  auto read_mesh = vfs.Read("meshes/sword.nif");
  Check("Vfs serves a streamed mesh",
        read_mesh && read_mesh->size() == mesh.size() &&
            std::equal(read_mesh->begin(), read_mesh->end(), mesh.begin()));
  auto read_sound = vfs.Read("sound/hit.wav");
  Check("Vfs serves a streamed sound",
        read_sound && read_sound->size() == sound.size());
  Check("Vfs reports a missing path absent", !vfs.Read("meshes/missing.nif"));

  fs::remove_all(tmp, ec);
  std::printf("modstreamtest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
