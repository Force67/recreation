// string_writertest: deterministic checks for the localized string table
// writer (StringTableWriter). It builds a table with StringTableWriter, then
// parses the bytes back with a minimal inline parser that mirrors
// StringTable::LoadFile (strings.cc), so it needs no game data and runs in the
// ctest gate. Both variants are covered: the plain .strings form (NUL
// terminated, no length prefix) and the .dlstrings/.ilstrings form (u32 length
// prefix that includes the terminator). A Save round trip through a temp file
// exercises the on-disk path too.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "bethesda/string_writer.h"
#include "core/types.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Minimal reimplementation of StringTable::LoadFile over a raw byte buffer,
// kept byte-for-byte in step with engine/bethesda/strings.cc so a pass here
// means the writer's layout is exactly what the real reader consumes.
std::map<u32, std::string> Parse(const std::vector<u8>& bytes, bool length_prefixed, bool* ok) {
  std::map<u32, std::string> out;
  *ok = false;
  if (bytes.size() < 8) return out;

  u32 count = 0, data_size = 0;
  std::memcpy(&count, bytes.data(), 4);
  std::memcpy(&data_size, bytes.data() + 4, 4);

  size_t directory_end = 8 + static_cast<size_t>(count) * 8;
  if (bytes.size() < directory_end + data_size) return out;

  for (u32 i = 0; i < count; ++i) {
    u32 id = 0, offset = 0;
    std::memcpy(&id, bytes.data() + 8 + i * 8, 4);
    std::memcpy(&offset, bytes.data() + 8 + i * 8 + 4, 4);
    size_t pos = directory_end + offset;
    if (pos >= bytes.size()) continue;

    const char* start = reinterpret_cast<const char*>(bytes.data() + pos);
    if (length_prefixed) {
      if (pos + 4 > bytes.size()) continue;
      u32 length = 0;
      std::memcpy(&length, start, 4);
      if (pos + 4 + length > bytes.size()) continue;
      out[id] = std::string(start + 4, length > 0 ? length - 1 : 0);
    } else {
      size_t max_length = bytes.size() - pos;
      out[id] = std::string(start, strnlen(start, max_length));
    }
  }
  *ok = true;
  return out;
}

std::vector<u8> ToVector(const base::Vector<u8>& v) {
  return std::vector<u8>(v.data(), v.data() + v.size());
}

std::vector<u8> ReadFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Runs a full add/build/parse round trip for one variant and asserts every id
// maps back to the exact original text.
void RunVariant(const char* label, bool length_prefixed, const std::string& dir) {
  std::printf("string table %s round trip:\n", label);

  // A mix of ordinary text, an empty string, repeated text (must dedup to one
  // data slot yet keep distinct ids), and high/non-ASCII bytes (no embedded
  // NUL, which neither variant can represent).
  const std::string s_hello = "Hello, courier.";
  const std::string s_empty = "";
  const std::string s_unicode = "caf\xC3\xA9 \xF0\x9F\x97\xA1";  // "café [dagger]"
  const std::string s_long(300, 'X');                            // spans a >255 length

  rec::bethesda::StringTableWriter w;
  u32 id_hello = w.Add(s_hello);
  u32 id_empty = w.Add(s_empty);
  u32 id_unicode = w.Add(s_unicode);
  u32 id_long = w.Add(s_long);
  u32 id_dup = w.Add(s_hello);  // same text as id_hello, new id

  Check("ids are sequential from 1",
        id_hello == 1 && id_empty == 2 && id_unicode == 3 && id_long == 4 && id_dup == 5);
  Check("size counts every id", w.size() == 5);

  base::Vector<u8> built = w.Build(length_prefixed);
  std::vector<u8> bytes = ToVector(built);

  // Header sanity: count field matches the number of directory entries.
  Check("has header", bytes.size() >= 8);
  u32 count = 0;
  if (bytes.size() >= 8) std::memcpy(&count, bytes.data(), 4);
  Check("header count == 5", count == 5);

  bool parsed_ok = false;
  std::map<u32, std::string> table = Parse(bytes, length_prefixed, &parsed_ok);
  Check("parses back", parsed_ok);

  Check("hello round trips", table[id_hello] == s_hello);
  Check("empty round trips", table.count(id_empty) == 1 && table[id_empty].empty());
  Check("unicode bytes preserved", table[id_unicode] == s_unicode);
  Check("long (>255) round trips", table[id_long] == s_long);
  Check("duplicate id maps to same text", table[id_dup] == s_hello);

  // The duplicate must share a data-block offset with the original.
  u32 off_hello = 0, off_dup = 0;
  auto offset_of = [&](u32 id, u32* off) {
    for (u32 i = 0; i < count; ++i) {
      u32 eid = 0;
      std::memcpy(&eid, bytes.data() + 8 + i * 8, 4);
      if (eid == id) {
        std::memcpy(off, bytes.data() + 8 + i * 8 + 4, 4);
        return true;
      }
    }
    return false;
  };
  bool got = offset_of(id_hello, &off_hello) && offset_of(id_dup, &off_dup);
  Check("identical strings share one data slot", got && off_hello == off_dup);

  // Save() must produce byte-identical output on disk.
  const std::string path = dir + "/rec_string_writertest_" + label + ".bin";
  Check("saves", w.Save(path, length_prefixed));
  std::vector<u8> disk = ReadFileBytes(path);
  Check("file bytes == Build bytes", disk == bytes);

  bool disk_ok = false;
  std::map<u32, std::string> from_disk = Parse(disk, length_prefixed, &disk_ok);
  Check("file parses back", disk_ok && from_disk[id_long] == s_long &&
                                from_disk[id_unicode] == s_unicode);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// Confirms Set() honors explicit ids and keeps auto-assignment past them.
void TestExplicitIds() {
  std::puts("string table explicit ids:");
  rec::bethesda::StringTableWriter w;
  w.Set(100, "hundred");
  w.Set(5, "five");
  w.Set(100, "hundred-updated");  // overwrite, no new entry
  u32 next = w.Add("auto");       // must be > 100

  Check("no duplicate entry on overwrite", w.size() == 3);
  Check("auto id follows highest explicit id", next == 101);

  bool ok = false;
  std::map<u32, std::string> table = Parse(ToVector(w.Build(true)), true, &ok);
  Check("explicit ids parse", ok);
  Check("id 100 overwritten", table[100] == "hundred-updated");
  Check("id 5 preserved", table[5] == "five");
  Check("auto id present", table[101] == "auto");
}

}  // namespace

int main() {
  const std::string dir = std::filesystem::temp_directory_path().string();
  RunVariant("strings", /*length_prefixed=*/false, dir);
  RunVariant("dlstrings", /*length_prefixed=*/true, dir);
  TestExplicitIds();

  if (g_failures == 0) {
    std::puts("string_writer: all checks passed");
    return 0;
  }
  std::printf("string_writer: %d checks FAILED\n", g_failures);
  return 1;
}
