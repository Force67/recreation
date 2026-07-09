// raw_rewritertest: checks for the structure-preserving rewriter
// (engine/bethesda/raw_rewriter.*). It authors plugins with the writer, then
// rewrites them: an unedited rewrite must be byte-identical (preserving
// compression, deleted records, group tree and TES4 verbatim), and record
// replace/delete must reload correctly with recomputed group sizes. No game
// data, runs in the ctest gate.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "bethesda/game_profile.h"
#include "bethesda/plugin.h"
#include "bethesda/raw_rewriter.h"
#include "bethesda/record.h"
#include "bethesda/writer.h"
#include "core/types.h"

using namespace rx;
using namespace rx::bethesda;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

constexpr u32 kWeap = FourCc('W', 'E', 'A', 'P');
constexpr u32 kArmo = FourCc('A', 'R', 'M', 'O');
constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');

bool BytesEqual(const base::Vector<u8>& a, const base::Vector<u8>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// Builds a small plugin: three records over two type groups. `compress` and a
// deleted record exercise the fidelity-sensitive paths.
base::Vector<u8> AuthorPlugin(const GameProfile& profile, bool compress) {
  PluginWriter writer(profile);
  writer.set_author("base").add_master("Skyrim.esm").set_compress(compress);

  RecordBuilder a(kWeap, RawFormId{0x01000800});
  a.EditorId("SwordA");
  writer.AddRecord(a.record());

  RecordBuilder b(kArmo, RawFormId{0x01000801});
  b.EditorId("HelmA");
  writer.AddRecord(b.record());

  RecordBuilder c(kWeap, RawFormId{0x01000802});
  c.EditorId("SwordB");
  writer.AddRecord(c.record());
  return writer.Build();
}

void TestByteIdentity(const GameProfile& profile, bool compress) {
  std::printf("byte-identity (%s):\n", compress ? "compressed" : "uncompressed");
  base::Vector<u8> original = AuthorPlugin(profile, compress);
  RawRewriter rewriter(std::move(original));
  base::Vector<u8> rebuilt = rewriter.Build();
  Check("unedited rewrite is byte-identical", BytesEqual(rewriter.bytes(), rebuilt));
}

// A plugin carrying a record with the deleted flag set (the loader would drop
// it on read; the rewriter must keep it).
void TestDeletedRecordPreserved(const GameProfile& profile) {
  std::printf("deleted-record preservation:\n");
  PluginWriter writer(profile);
  writer.add_master("Skyrim.esm");
  RecordBuilder live(kWeap, RawFormId{0x01000800});
  live.EditorId("Live");
  writer.AddRecord(live.record());
  // A deleted record: same shape but flagged deleted.
  RecordBuilder gone(kWeap, RawFormId{0x01000801}, kRecordFlagDeleted);
  gone.EditorId("Gone");
  writer.AddRecord(gone.record());

  base::Vector<u8> original = writer.Build();
  RawRewriter rewriter(std::move(original));
  Check("rewrite keeps the deleted record verbatim",
        BytesEqual(rewriter.bytes(), rewriter.Build()));
}

void TestReplaceAndDelete(const std::string& dir, const GameProfile& profile) {
  std::printf("replace + delete:\n");
  base::Vector<u8> original = AuthorPlugin(profile, /*compress=*/false);
  RawRewriter rewriter(std::move(original));

  // Replace SwordA (0x01000800) with a bigger encoded record (longer editor id
  // + an extra field) to force enclosing group-size recomputation.
  RecordBuilder bigger(kWeap, RawFormId{0x01000800});
  bigger.EditorId("SwordA_Reforged_With_A_Much_Longer_Name");
  u32 extra = 0x12345678;
  bigger.FieldPod(FourCc('D', 'A', 'T', 'A'), extra);
  base::Vector<u8> encoded;
  EncodeRecord(bigger.record(), &encoded);
  rewriter.Replace(0x01000800, std::move(encoded));

  // Delete HelmA (0x01000801).
  rewriter.Delete(0x01000801);

  const std::string path = dir + "/rec_rewrite.esp";
  Check("save rewritten", rewriter.Save(path));

  const GameProfile& p = profile;
  auto plugin = PluginFile::Open(path, p);
  Check("rewritten plugin opens", plugin.has_value());
  if (!plugin) return;

  std::vector<std::string> weap_edids;
  bool saw_armo = false;
  bool structure_ok = true;
  plugin->VisitRecords([&](Record& r) {
    if (r.header.type == kArmo) saw_armo = true;
    if (r.header.type == kWeap) weap_edids.push_back(r.GetString(kEdid));
  });
  // If any group size were wrong, the walk would desync and mis-parse; a clean
  // read of exactly the expected records is the structural check.
  Check("HelmA (ARMO) is gone", !saw_armo);
  Check("both swords remain", weap_edids.size() == 2);
  bool has_reforged = false, has_swordb = false;
  for (const std::string& e : weap_edids) {
    if (e == "SwordA_Reforged_With_A_Much_Longer_Name") has_reforged = true;
    if (e == "SwordB") has_swordb = true;
  }
  Check("replacement content present", has_reforged);
  Check("untouched record intact", has_swordb);
  (void)structure_ok;

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
  const std::string dir = std::filesystem::temp_directory_path().string();
  const GameProfile& profile = GameProfile::For(Game::kSkyrimSe);

  TestByteIdentity(profile, /*compress=*/false);
  TestByteIdentity(profile, /*compress=*/true);
  TestDeletedRecordPreserved(profile);
  TestReplaceAndDelete(dir, profile);

  if (g_failures == 0) {
    std::puts("raw_rewriter: all checks passed");
    return 0;
  }
  std::printf("raw_rewriter: %d checks FAILED\n", g_failures);
  return 1;
}
