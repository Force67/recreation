// ba2test: deterministic checks for the BA2 archive reader (Fallout 4/76). It
// builds minimal synthetic GNRL and DX10 archives in a temp file and reads them
// back through OpenArchive, so it needs no game data and runs in the ctest gate.
// Both paths use uncompressed entries (packed size 0), so no zlib stream is
// involved; the codec itself is covered by reading the shipped archives.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bethesda/archive.h"
#include "core/types.h"

// zetanet's headers (via archive.h) inject global arch_types scalar aliases, so
// the scalar types are left bare (they resolve to those same uint aliases) and
// the rec:: symbols are fully qualified, matching the other net/bethesda tests.
namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void PutU16(std::vector<u8>& b, u16 v) { b.insert(b.end(), {u8(v), u8(v >> 8)}); }
void PutU32(std::vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (8 * i)));
}
void PutU64(std::vector<u8>& b, u64 v) {
  for (int i = 0; i < 8; ++i) b.push_back(u8(v >> (8 * i)));
}
void PutBytes(std::vector<u8>& b, const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i) b.push_back(static_cast<u8>(s[i]));
}

// Writes `bytes` to a temp .ba2 and opens it through OpenArchive.
base::UniquePointer<rec::asset::FileProvider> OpenSynthetic(const std::vector<u8>& bytes,
                                                            const std::string& path) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  return rec::bethesda::OpenArchive(path);
}

void TestGnrl(const std::string& dir) {
  std::puts("ba2 GNRL round trip:");
  const std::vector<u8> payload = {0xDE, 0xAD, 0xBE, 0xEF, 'n', 'i', 'f', 0x01};
  const std::string name = "Meshes\\Test\\Thing.nif";  // backslash, mixed case

  constexpr u64 kHeaderSize = 24;
  constexpr u64 kRecordSize = 36;
  const u64 data_offset = kHeaderSize + kRecordSize;
  const u64 name_table_offset = data_offset + payload.size();

  std::vector<u8> b;
  PutBytes(b, "BTDX", 4);
  PutU32(b, 1);  // version
  PutBytes(b, "GNRL", 4);
  PutU32(b, 1);                  // file count
  PutU64(b, name_table_offset);  // 24-byte header complete

  // 36-byte GNRL record.
  PutU32(b, 0);  // name hash
  PutBytes(b, "nif\0", 4);
  PutU32(b, 0);  // dir hash
  PutU32(b, 0);  // flags
  PutU64(b, data_offset);
  PutU32(b, 0);  // packed size 0 => stored uncompressed
  PutU32(b, static_cast<u32>(payload.size()));
  PutU32(b, 0xBAADF00D);

  b.insert(b.end(), payload.begin(), payload.end());  // file data

  PutU16(b, static_cast<u16>(name.size()));  // name table
  PutBytes(b, name.data(), name.size());

  auto provider = OpenSynthetic(b, dir + "/rec_ba2test_gnrl.ba2");
  Check("opens", provider != nullptr);
  if (!provider) return;

  // Lookups are by the normalized (lower-case, forward-slash) path.
  const std::string key = "meshes/test/thing.nif";
  Check("contains by normalized path", provider->Contains(key));
  Check("missing path absent", !provider->Contains("meshes/nope.nif"));

  auto data = provider->Read(key);
  Check("reads", data.has_value());
  if (data) {
    Check("payload length", data->size() == payload.size());
    Check("payload bytes match",
          data->size() == payload.size() &&
              std::memcmp(data->data(), payload.data(), payload.size()) == 0);
  }
}

void TestDx10(const std::string& dir) {
  std::puts("ba2 DX10 round trip:");
  // One 4x4 BC7 block (16 bytes) as the single mip-0 chunk.
  std::vector<u8> mip0(16);
  for (size_t i = 0; i < mip0.size(); ++i) mip0[i] = static_cast<u8>(i + 1);
  const std::string name = "Textures\\Test\\Albedo_d.DDS";

  constexpr u64 kHeaderSize = 24;
  constexpr u64 kTexHeaderSize = 24;
  constexpr u64 kChunkSize = 24;
  const u64 chunk_data_offset = kHeaderSize + kTexHeaderSize + kChunkSize;
  const u64 name_table_offset = chunk_data_offset + mip0.size();
  constexpr u16 kWidth = 4, kHeight = 4;
  constexpr u8 kDxgiBc7 = 98;

  std::vector<u8> b;
  PutBytes(b, "BTDX", 4);
  PutU32(b, 7);  // version (texture archives ship v7/8)
  PutBytes(b, "DX10", 4);
  PutU32(b, 1);
  PutU64(b, name_table_offset);

  // 24-byte tex header.
  PutU32(b, 0);  // name hash
  PutBytes(b, "dds\0", 4);
  PutU32(b, 0);  // dir hash
  b.push_back(0);             // unknown
  b.push_back(1);             // chunk count
  PutU16(b, kChunkSize);      // chunk header size
  PutU16(b, kHeight);
  PutU16(b, kWidth);
  b.push_back(1);             // mip count
  b.push_back(kDxgiBc7);      // dxgi format
  b.push_back(0);             // is cubemap
  b.push_back(0);             // tile mode

  // 24-byte chunk.
  PutU64(b, chunk_data_offset);
  PutU32(b, 0);  // packed size 0 => uncompressed
  PutU32(b, static_cast<u32>(mip0.size()));
  PutU16(b, 0);  // start mip
  PutU16(b, 0);  // end mip
  PutU32(b, 0xBAADF00D);

  b.insert(b.end(), mip0.begin(), mip0.end());  // chunk data

  PutU16(b, static_cast<u16>(name.size()));
  PutBytes(b, name.data(), name.size());

  auto provider = OpenSynthetic(b, dir + "/rec_ba2test_dx10.ba2");
  Check("opens", provider != nullptr);
  if (!provider) return;

  const std::string key = "textures/test/albedo_d.dds";
  Check("contains by normalized path", provider->Contains(key));
  auto dds = provider->Read(key);
  Check("reads", dds.has_value());
  if (!dds) return;

  // The reader synthesizes a DDS header (4-byte magic + 124 header + 20 DX10),
  // so the chunk data lands at byte 148. Fields line up with what ConvertDds
  // reads: magic@0, height@12, width@16, dxgi@128, data@148.
  Check("dds size = 148 + mip", dds->size() == 148 + mip0.size());
  Check("dds magic", dds->size() >= 4 && std::memcmp(dds->data(), "DDS ", 4) == 0);
  auto u32_at = [&](size_t off) {
    u32 v = 0;
    if (off + 4 <= dds->size()) std::memcpy(&v, dds->data() + off, 4);
    return v;
  };
  Check("dds height", u32_at(12) == kHeight);
  Check("dds width", u32_at(16) == kWidth);
  Check("dds dxgi format", u32_at(128) == kDxgiBc7);
  Check("mip data preserved at 148",
        dds->size() == 148 + mip0.size() &&
            std::memcmp(dds->data() + 148, mip0.data(), mip0.size()) == 0);
}

// Starfield (BA2 v2/v3) keeps the FO4 record layout but inserts a u64 between
// the 24-byte header and the first record. The reader must skip it, otherwise
// every offset is 8 bytes short. This builds a v2 GNRL archive with that field
// and confirms a clean read.
void TestStarfieldGnrl(const std::string& dir) {
  std::puts("ba2 Starfield (v2) GNRL round trip:");
  const std::vector<u8> payload = {'m', 'e', 's', 'h', 0x02, 0x00, 0x00, 0x00};
  const std::string name = "Geometries\\ABCD\\1234.mesh";

  constexpr u64 kHeaderSize = 24;
  constexpr u64 kExtraHeaderSize = 8;  // the v2/v3 u64
  constexpr u64 kRecordSize = 36;
  const u64 data_offset = kHeaderSize + kExtraHeaderSize + kRecordSize;
  const u64 name_table_offset = data_offset + payload.size();

  std::vector<u8> b;
  PutBytes(b, "BTDX", 4);
  PutU32(b, 2);  // version (Starfield general archives ship v2)
  PutBytes(b, "GNRL", 4);
  PutU32(b, 1);
  PutU64(b, name_table_offset);
  PutU64(b, 1);  // v2/v3 extra header field, skipped by the reader

  PutU32(b, 0);  // name hash
  PutBytes(b, "mesh", 4);
  PutU32(b, 0);  // dir hash
  PutU32(b, 0x00100100);  // flags as seen in shipped archives
  PutU64(b, data_offset);
  PutU32(b, 0);  // packed size 0 => stored uncompressed
  PutU32(b, static_cast<u32>(payload.size()));
  PutU32(b, 0xBAADF00D);

  b.insert(b.end(), payload.begin(), payload.end());

  PutU16(b, static_cast<u16>(name.size()));
  PutBytes(b, name.data(), name.size());

  auto provider = OpenSynthetic(b, dir + "/rec_ba2test_sf.ba2");
  Check("opens", provider != nullptr);
  if (!provider) return;

  const std::string key = "geometries/abcd/1234.mesh";
  Check("contains by normalized path", provider->Contains(key));
  auto data = provider->Read(key);
  Check("reads", data.has_value());
  if (data) {
    Check("payload length", data->size() == payload.size());
    Check("payload bytes match",
          data->size() == payload.size() &&
              std::memcmp(data->data(), payload.data(), payload.size()) == 0);
  }
}

// Starfield v3 texture archives put a u64 AND a u32 (12 bytes) between the
// header and the records, where v2 has only the u64. Reading the v2 size here
// would land on garbage texture dimensions; this confirms the v3 offset.
void TestStarfieldV3Dx10(const std::string& dir) {
  std::puts("ba2 Starfield (v3) DX10 round trip:");
  std::vector<u8> mip0(16);
  for (size_t i = 0; i < mip0.size(); ++i) mip0[i] = static_cast<u8>(i + 1);
  const std::string name = "Textures\\Land\\Rock_color.DDS";

  constexpr u64 kHeaderSize = 24;
  constexpr u64 kExtraHeaderSize = 12;  // v3: u64 + u32
  constexpr u64 kTexHeaderSize = 24;
  constexpr u64 kChunkSize = 24;
  const u64 chunk_data_offset = kHeaderSize + kExtraHeaderSize + kTexHeaderSize + kChunkSize;
  const u64 name_table_offset = chunk_data_offset + mip0.size();
  constexpr u16 kWidth = 4, kHeight = 4;
  constexpr u8 kDxgiBc7 = 98;

  std::vector<u8> b;
  PutBytes(b, "BTDX", 4);
  PutU32(b, 3);  // version
  PutBytes(b, "DX10", 4);
  PutU32(b, 1);
  PutU64(b, name_table_offset);
  PutU64(b, 1);  // v2/v3 u64
  PutU32(b, 3);  // v3-only u32

  PutU32(b, 0);  // name hash
  PutBytes(b, "dds\0", 4);
  PutU32(b, 0);  // dir hash
  b.push_back(0);
  b.push_back(1);             // chunk count
  PutU16(b, kChunkSize);
  PutU16(b, kHeight);
  PutU16(b, kWidth);
  b.push_back(1);             // mip count
  b.push_back(kDxgiBc7);
  b.push_back(0);
  b.push_back(0);

  PutU64(b, chunk_data_offset);
  PutU32(b, 0);  // uncompressed
  PutU32(b, static_cast<u32>(mip0.size()));
  PutU16(b, 0);
  PutU16(b, 0);
  PutU32(b, 0xBAADF00D);

  b.insert(b.end(), mip0.begin(), mip0.end());

  PutU16(b, static_cast<u16>(name.size()));
  PutBytes(b, name.data(), name.size());

  auto provider = OpenSynthetic(b, dir + "/rec_ba2test_sf_v3.ba2");
  Check("opens", provider != nullptr);
  if (!provider) return;
  auto dds = provider->Read("textures/land/rock_color.dds");
  Check("reads", dds.has_value());
  if (!dds) return;
  auto u32_at = [&](size_t off) {
    u32 v = 0;
    if (off + 4 <= dds->size()) std::memcpy(&v, dds->data() + off, 4);
    return v;
  };
  Check("dds height", u32_at(12) == kHeight);
  Check("dds width", u32_at(16) == kWidth);
  Check("mip data preserved", dds->size() == 148 + mip0.size() &&
                                  std::memcmp(dds->data() + 148, mip0.data(), mip0.size()) == 0);
}

}  // namespace

int main() {
  const std::string dir = std::filesystem::temp_directory_path().string();
  TestGnrl(dir);
  TestDx10(dir);
  TestStarfieldGnrl(dir);
  TestStarfieldV3Dx10(dir);
  std::error_code ec;
  std::filesystem::remove(dir + "/rec_ba2test_gnrl.ba2", ec);
  std::filesystem::remove(dir + "/rec_ba2test_dx10.ba2", ec);
  std::filesystem::remove(dir + "/rec_ba2test_sf.ba2", ec);
  std::filesystem::remove(dir + "/rec_ba2test_sf_v3.ba2", ec);
  if (g_failures == 0) {
    std::puts("ba2: all checks passed");
    return 0;
  }
  std::printf("ba2: %d checks FAILED\n", g_failures);
  return 1;
}
