// writertest: deterministic checks for the plugin writer (engine/bethesda/
// writer.*). It builds records with RecordBuilder, encodes them, and reads them
// back through the same parser the loader uses (ParseRecordPayload) and through
// PluginFile::Open, so it needs no game data and runs in the ctest gate. It
// proves the writer is the inverse of the reader: parse -> encode -> parse is
// stable and, for an uncompressed record, byte identical.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "bethesda/compression.h"
#include "bethesda/game_profile.h"
#include "bethesda/plugin.h"
#include "bethesda/writer.h"
#include "core/types.h"

using namespace rec;
using namespace rec::bethesda;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

constexpr u32 kWeap = FourCc('W', 'E', 'A', 'P');
constexpr u32 kArmo = FourCc('A', 'R', 'M', 'O');
constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kBigx = FourCc('B', 'I', 'G', 'X');

// Encodes a record, parses it back, and checks fields survive. Also exercises
// the XXXX escape with a field larger than 0xffff bytes.
void TestRecordRoundTrip() {
  std::printf("record round-trip:\n");
  RawFormId fid{0x00000800};
  RecordBuilder builder(kWeap, fid);
  builder.EditorId("TestSword");
  u32 data_value = 0xdeadbeef;
  builder.FieldPod(kData, data_value);
  std::vector<u8> big(70000, 0xab);  // > 0xffff, forces an XXXX escape
  builder.Field(kBigx, ByteSpan(big.data(), big.size()));

  base::Vector<u8> encoded;
  EncodeRecord(builder.record(), &encoded);
  Check("encoded has header + payload", encoded.size() > sizeof(RecordHeader));

  RecordHeader header{};
  std::memcpy(&header, encoded.data(), sizeof(header));
  ByteSpan payload(encoded.data() + sizeof(RecordHeader), encoded.size() - sizeof(RecordHeader));
  Check("data_size matches payload", header.data_size == payload.size());

  Record parsed;
  Check("parse re-encoded record", ParseRecordPayload(header, payload, &parsed));
  Check("type preserved", parsed.header.type == kWeap);
  Check("form id preserved", parsed.header.form_id.value == fid.value);
  Check("subrecord count", parsed.subrecords.size() == 3);
  Check("edid round-trips", parsed.GetString(kEdid) == "TestSword");
  const Subrecord* d = parsed.Find(kData);
  Check("data field", d && d->data.size() == 4 && std::memcmp(d->data.data(), &data_value, 4) == 0);
  const Subrecord* b = parsed.Find(kBigx);
  Check("xxxx field size", b && b->data.size() == big.size());
  Check("xxxx field bytes", b && b->data.size() == big.size() && b->data[0] == 0xab &&
                                b->data[big.size() - 1] == 0xab);

  // parse -> encode must be byte identical (uncompressed round-trip).
  base::Vector<u8> reencoded;
  EncodeRecord(parsed, &reencoded);
  Check("re-encode byte identical",
        reencoded.size() == encoded.size() &&
            std::memcmp(reencoded.data(), encoded.data(), encoded.size()) == 0);
}

// Writes a whole plugin and reads it back through the real PluginFile loader.
void TestPluginRoundTrip(const std::string& dir) {
  std::printf("plugin round-trip:\n");
  const GameProfile& profile = GameProfile::For(Game::kSkyrimSe);

  PluginWriter writer(profile);
  writer.set_author("recreation").add_master("Skyrim.esm");
  RecordBuilder sword_a(kWeap, RawFormId{0x01000800});
  sword_a.EditorId("SwordA");
  RecordBuilder helm_a(kArmo, RawFormId{0x01000801});
  helm_a.EditorId("HelmA");
  RecordBuilder sword_b(kWeap, RawFormId{0x01000802});
  sword_b.EditorId("SwordB");
  writer.AddRecord(sword_a.record());
  writer.AddRecord(helm_a.record());
  writer.AddRecord(sword_b.record());

  const std::string path = dir + "/rec_writertest.esp";
  Check("save", writer.Save(path));

  auto plugin = PluginFile::Open(path, profile);
  Check("reopen", plugin.has_value());
  if (!plugin) return;
  Check("masters parsed", plugin->masters().size() == 1 && plugin->masters()[0] == "Skyrim.esm");
  Check("hedr record count", plugin->record_count() == 3);

  std::vector<std::pair<u32, std::string>> got;
  plugin->VisitRecords(
      [&](Record& r) { got.emplace_back(r.header.type, r.GetString(kEdid)); });
  Check("visited 3 records", got.size() == 3);
  // Groups emit in first-seen type order (WEAP then ARMO); records keep
  // insertion order within a group.
  Check("order: WEAP SwordA", got.size() > 0 && got[0].first == kWeap && got[0].second == "SwordA");
  Check("order: WEAP SwordB", got.size() > 1 && got[1].first == kWeap && got[1].second == "SwordB");
  Check("order: ARMO HelmA", got.size() > 2 && got[2].first == kArmo && got[2].second == "HelmA");

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// Compresses records and reads them back through the loader's decompress path.
void TestCompression(const std::string& dir) {
  std::printf("compression:\n");
  // Direct codec round-trip on data with a repeated pattern.
  std::vector<u8> raw(5000);
  for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<u8>(i * 7 + 3);
  base::Vector<u8> stream = ZlibDeflateStored(ByteSpan(raw.data(), raw.size()));
  std::vector<u8> back(raw.size());
  Check("inflate(deflate(x)) succeeds",
        ZlibInflate(ByteSpan(stream.data(), stream.size()), back.data(), back.size()));
  Check("inflate(deflate(x)) == x", std::memcmp(back.data(), raw.data(), raw.size()) == 0);

  // Whole-plugin round-trip with the compressed flag through PluginFile.
  const GameProfile& profile = GameProfile::For(Game::kSkyrimSe);
  PluginWriter writer(profile);
  writer.set_compress(true);
  RecordBuilder book(FourCc('B', 'O', 'O', 'K'), RawFormId{0x00000800});
  book.EditorId("BigBook");
  book.Field(FourCc('D', 'E', 'S', 'C'), ByteSpan(raw.data(), raw.size()));
  writer.AddRecord(book.record());

  const std::string path = dir + "/rec_writertest_z.esp";
  Check("save compressed", writer.Save(path));

  auto plugin = PluginFile::Open(path, profile);
  Check("reopen compressed", plugin.has_value());
  bool found = false;
  bool desc_ok = false;
  if (plugin) {
    plugin->VisitRecords([&](Record& r) {
      if (r.header.type != FourCc('B', 'O', 'O', 'K')) return;
      found = true;
      const Subrecord* desc = r.Find(FourCc('D', 'E', 'S', 'C'));
      desc_ok = desc && desc->data.size() == raw.size() &&
                std::memcmp(desc->data.data(), raw.data(), raw.size()) == 0;
      // The record on disk carried the compressed flag.
      desc_ok = desc_ok && (r.header.flags & kRecordFlagCompressed) != 0;
    });
  }
  Check("compressed record decodes", found && desc_ok);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
  const std::string dir = std::filesystem::temp_directory_path().string();
  TestRecordRoundTrip();
  TestPluginRoundTrip(dir);
  TestCompression(dir);
  if (g_failures == 0) {
    std::puts("writer: all checks passed");
    return 0;
  }
  std::printf("writer: %d checks FAILED\n", g_failures);
  return 1;
}
