// roundtriptest: fidelity harness for the plugin reader<->writer pair. It
// proves that parsing a record and handing it straight back to the writer is a
// true byte-level inverse for the uncompressed/flat records the P0 writer
// emits, and precisely bounds where full byte-faithfulness still stops.
//
// Two sections:
//   (1) A CI-safe synthetic round trip. It authors a plugin in memory with
//       PluginWriter/RecordBuilder (several records, several types, multiple
//       subrecords each, and one >0xffff field to exercise the XXXX escape),
//       saves it, reopens it, and for every record asserts EncodeRecord
//       reproduces the exact on-disk bytes (24-byte header + payload) the reader
//       loaded. This runs always, needs no game data, and gates in ctest.
//   (2) A data-dir-gated real-plugin fidelity pass. Given a Skyrim Data dir as
//       argv[1] it loads the base masters through RecordStore and, for a sample
//       of common record types, re-encodes each parsed record and classifies it
//       as byte-identical (uncompressed) or structurally-identical (originally
//       compressed, where bytes legitimately differ because P0 stores). Missing
//       data never fails CI: it prints a skip and returns 0.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "bethesda/game_profile.h"
#include "bethesda/load_order.h"
#include "bethesda/plugin.h"
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
constexpr u32 kMisc = FourCc('M', 'I', 'S', 'C');
constexpr u32 kArmo = FourCc('A', 'R', 'M', 'O');
constexpr u32 kCell = FourCc('C', 'E', 'L', 'L');
constexpr u32 kNpc = FourCc('N', 'P', 'C', '_');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kFull = FourCc('F', 'U', 'L', 'L');
constexpr u32 kModl = FourCc('M', 'O', 'D', 'L');
constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');

// A game profile with no forced base masters, so the synthetic plugin loads on
// its own and PluginFile::Open is happy without shipped game files.
GameProfile TestProfile() {
  GameProfile profile;
  profile.game = Game::kSkyrimSe;
  profile.name = "test";
  profile.plugin_version = 1.0f;
  return profile;
}

// One field the synthetic writer adds and the reader must reproduce exactly.
struct ExpectedField {
  u32 type = 0;
  std::vector<u8> bytes;
};

// The whole expected content of one authored record, keyed by form id, so the
// out-of-add-order (grouped-by-type) file walk can match each record back.
struct ExpectedRecord {
  u32 type = 0;
  std::vector<ExpectedField> fields;
};

// Compares parsed subrecords against what was authored, positionally: the
// writer preserves field order, so encode->parse must too.
bool FieldsMatch(const base::Vector<Subrecord>& got, const std::vector<ExpectedField>& want) {
  if (got.size() != want.size()) return false;
  for (size_t i = 0; i < want.size(); ++i) {
    if (got[i].type != want[i].type) return false;
    if (got[i].data.size() != want[i].bytes.size()) return false;
    if (!want[i].bytes.empty() &&
        std::memcmp(got[i].data.data(), want[i].bytes.data(), want[i].bytes.size()) != 0) {
      return false;
    }
  }
  return true;
}

// A zero-terminated FULL name field, authored the same way the reader keeps it.
ExpectedField MakeFull(const char* s) {
  ExpectedField f{kFull, {}};
  for (const char* p = s; *p; ++p) f.bytes.push_back(static_cast<u8>(*p));
  f.bytes.push_back(0);
  return f;
}

// -------------------------------------------------------------------------
// (1) Synthetic byte-identity round trip.
// -------------------------------------------------------------------------
void SyntheticRoundTrip(const std::string& dir) {
  std::puts("synthetic byte-identity round trip:");
  const std::string path = dir + "/rec_roundtrip.esp";
  const GameProfile profile = TestProfile();

  std::unordered_map<u32, ExpectedRecord> expected;  // form-id value -> content

  PluginWriter writer(profile);
  writer.set_author("roundtrip").set_master(false).add_master("Skyrim.esm");

  // Helper: author a record with the given fields and remember what we wrote.
  auto author = [&](u32 rec_type, u32 form_value, const std::vector<ExpectedField>& fields) {
    RecordBuilder builder(rec_type, RawFormId{form_value});
    // Keep the field bytes alive for the duration of AddRecord via `fields`.
    for (const ExpectedField& f : fields) {
      if (f.type == kEdid) {
        // Author EDID through the string helper the reader expects (NUL kept).
        std::string id(reinterpret_cast<const char*>(f.bytes.data()),
                       f.bytes.empty() ? 0 : f.bytes.size() - 1);
        builder.EditorId(id);
      } else {
        builder.Field(f.type, ByteSpan(f.bytes.data(), f.bytes.size()));
      }
    }
    writer.AddRecord(builder.record());
    expected[form_value] = ExpectedRecord{rec_type, fields};
  };

  // A zero-terminated EDID field (matches RecordBuilder::EditorId output).
  auto edid = [](const char* s) {
    ExpectedField f{kEdid, {}};
    for (const char* p = s; *p; ++p) f.bytes.push_back(static_cast<u8>(*p));
    f.bytes.push_back(0);
    return f;
  };
  auto pod = [](u32 type, const std::vector<u8>& b) { return ExpectedField{type, b}; };

  // WEAP #1: EDID + a 10-byte DATA + a small FULL. Multiple subrecords.
  author(kWeap, 0x00000800,
         {edid("IronSword"), pod(kData, {10, 0, 0, 0, 0, 0, 0, 65, 7, 0}), MakeFull("Iron Sword")});
  // WEAP #2: same type, second record in the group.
  author(kWeap, 0x00000801, {edid("SteelSword"), pod(kData, {20, 0, 0, 0, 0, 0, 32, 65, 9, 0})});

  // MISC: a second top-level type, single small field.
  author(kMisc, 0x00000802, {edid("Gold"), pod(kData, {1, 0, 0, 0})});

  // ARMO: carries a large MODL (>0xffff) to force the XXXX escape on both the
  // encode and re-parse sides, plus ordinary fields around it so the escape is
  // exercised mid-record.
  {
    std::vector<u8> big(0x10001);  // 65537 bytes, one past the u16 boundary
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<u8>(i * 31 + 7);
    author(kArmo, 0x00000803,
           {edid("BigArmor"), pod(kModl, big), pod(kData, {5, 0, 0, 0, 0, 0, 0, 0})});
  }

  Check("save synthetic plugin", writer.Save(path));

  auto plugin = PluginFile::Open(path, profile);
  Check("reopen synthetic plugin", plugin.has_value());
  if (!plugin) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return;
  }

  // HEDR record count should equal what we authored (4 records).
  Check("HEDR record_count == 4", plugin->record_count() == expected.size());

  size_t walked = 0;
  size_t byte_identical = 0;
  bool all_matched_content = true;
  bool all_byte_identical = true;

  plugin->VisitRecordsRaw([&](const RecordHeader& header, ByteSpan payload, const GroupContext&) {
    ++walked;

    // Parse the raw payload into subrecords.
    Record parsed;
    if (!ParseRecordPayload(header, payload, &parsed)) {
      all_matched_content = false;
      all_byte_identical = false;
      return;
    }

    // Structural check: parsed subrecords must equal what we authored.
    auto it = expected.find(header.form_id.value);
    if (it == expected.end() || parsed.header.type != it->second.type ||
        !FieldsMatch(parsed.subrecords, it->second.fields)) {
      all_matched_content = false;
    }

    // Byte-identity check: EncodeRecord must reproduce the exact on-disk record,
    // i.e. the 24-byte header verbatim followed by the original payload bytes.
    auto encoded = base::Vector<u8>{};
    EncodeRecord(parsed, &encoded);

    std::vector<u8> original;
    original.resize(sizeof(RecordHeader) + payload.size());
    std::memcpy(original.data(), &header, sizeof(RecordHeader));
    if (!payload.empty()) {
      std::memcpy(original.data() + sizeof(RecordHeader), payload.data(), payload.size());
    }

    const bool identical = encoded.size() == original.size() &&
                           std::memcmp(encoded.data(), original.data(), original.size()) == 0;
    if (identical) {
      ++byte_identical;
    } else {
      all_byte_identical = false;
    }
  });

  Check("walked all 4 records", walked == expected.size());
  Check("every record's subrecords round-trip", all_matched_content);
  Check("every record re-encodes byte-identically", all_byte_identical);
  Check("byte-identical count == record count", byte_identical == expected.size());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// -------------------------------------------------------------------------
// (2) Data-dir-gated real-plugin fidelity.
// -------------------------------------------------------------------------

// Re-parses an EncodeRecord result (header + payload) back into subrecords, so
// originally-compressed records (whose bytes legitimately differ after P0
// stores) can still be checked for structural fidelity.
bool ReparseEncoded(const base::Vector<u8>& encoded, Record* out) {
  if (encoded.size() < sizeof(RecordHeader)) return false;
  RecordHeader header;
  std::memcpy(&header, encoded.data(), sizeof(RecordHeader));
  ByteSpan payload(encoded.data() + sizeof(RecordHeader), encoded.size() - sizeof(RecordHeader));
  return ParseRecordPayload(header, payload, out);
}

bool SubrecordsEqual(const base::Vector<Subrecord>& a, const base::Vector<Subrecord>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].type != b[i].type || a[i].data.size() != b[i].data.size()) return false;
    if (a[i].data.size() &&
        std::memcmp(a[i].data.data(), b[i].data.data(), a[i].data.size()) != 0) {
      return false;
    }
  }
  return true;
}

void RealPluginFidelity(const std::string& data_dir) {
  std::puts("real-plugin fidelity:");
  namespace fs = std::filesystem;

  const Game game = GameProfile::DetectFromDataDir(data_dir);
  if (game == Game::kUnknown) {
    std::puts("  (no recognized master in data dir, skipping)");
    return;
  }
  const GameProfile& profile = GameProfile::For(game);

  // Build a load order from the base masters that actually exist on disk. This
  // avoids depending on a plugins.txt location and keeps the pass defensive.
  LoadOrder order;
  size_t appended = 0;
  for (const std::string& master : profile.base_masters) {
    if (fs::exists(fs::path(data_dir) / master)) {
      order.Append(master);
      ++appended;
    }
  }
  if (appended == 0) {
    std::puts("  (no base masters present on disk, skipping)");
    return;
  }

  RecordStore store;
  if (!store.LoadAll(data_dir, order, profile)) {
    std::puts("  (RecordStore::LoadAll failed, likely a missing master; skipping)");
    return;
  }
  std::printf("  loaded %zu winning records from %zu master(s)\n", store.record_count(), appended);

  const u32 kSampleTypes[] = {kWeap, kArmo, kCell, kNpc};
  constexpr size_t kPerType = 50;  // sample a bounded slice per type

  size_t total_sampled = 0;
  size_t byte_identical = 0;   // uncompressed, exact match
  size_t structural_only = 0;  // originally compressed, structure preserved
  size_t byte_mismatch = 0;    // uncompressed but bytes differed (a real gap)
  size_t structural_fail = 0;  // structure did not survive the round trip
  size_t parse_failures = 0;   // could not parse at all

  for (u32 type : kSampleTypes) {
    size_t seen = 0;
    store.EachOfType(type, [&](GlobalFormId id, const RecordStore::StoredRecord& stored) {
      if (seen >= kPerType) return;
      ++seen;
      ++total_sampled;

      Record parsed;
      if (!store.Parse(id, &parsed)) {
        ++parse_failures;
        return;
      }

      auto encoded = base::Vector<u8>{};
      EncodeRecord(parsed, &encoded);

      const bool was_compressed = (stored.header.flags & kRecordFlagCompressed) != 0;
      if (!was_compressed) {
        // Original was stored uncompressed: encode must reproduce it exactly.
        std::vector<u8> original;
        original.resize(sizeof(RecordHeader) + stored.payload.size());
        std::memcpy(original.data(), &stored.header, sizeof(RecordHeader));
        if (!stored.payload.empty()) {
          std::memcpy(original.data() + sizeof(RecordHeader), stored.payload.data(),
                      stored.payload.size());
        }
        const bool identical = encoded.size() == original.size() &&
                               std::memcmp(encoded.data(), original.data(), original.size()) == 0;
        if (identical) {
          ++byte_identical;
        } else {
          ++byte_mismatch;
        }
      } else {
        // Original was compressed: P0 stores uncompressed, so bytes differ by
        // design. Verify structure survives by re-parsing the encoded record.
        Record reparsed;
        if (ReparseEncoded(encoded, &reparsed) &&
            SubrecordsEqual(parsed.subrecords, reparsed.subrecords)) {
          ++structural_only;
        } else {
          ++structural_fail;
        }
      }
    });
  }

  std::printf("  sampled=%zu  byte_identical=%zu  structural_only=%zu\n", total_sampled,
              byte_identical, structural_only);
  std::printf("  byte_mismatch=%zu  structural_fail=%zu  parse_failures=%zu\n", byte_mismatch,
              structural_fail, parse_failures);

  // Fidelity assertions: any uncompressed record MUST re-encode byte-identical,
  // and any record's structure MUST survive parse->encode->parse. A nonzero
  // byte_mismatch or structural_fail is a genuine regression, not a P0 gap.
  Check("sampled at least one record", total_sampled > 0);
  Check("no uncompressed byte mismatches", byte_mismatch == 0);
  Check("no structural round-trip failures", structural_fail == 0);
  // parse_failures are tolerated but reported: some record types carry payload
  // shapes the generic subrecord splitter rejects (e.g. odd trailing bytes).
  if (parse_failures > 0) {
    std::printf("  note: %zu sampled records failed to parse (reported, not fatal)\n",
                parse_failures);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = std::filesystem::temp_directory_path().string();

  SyntheticRoundTrip(dir);

  if (argc > 1) {
    RealPluginFidelity(argv[1]);
  } else {
    std::puts("roundtrip: no data dir, skipping real-plugin checks");
  }

  if (g_failures == 0) {
    std::puts("roundtrip: all checks passed");
    return 0;
  }
  std::printf("roundtrip: %d checks FAILED\n", g_failures);
  return 1;
}
