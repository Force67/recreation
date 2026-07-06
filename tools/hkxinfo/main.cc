// hkxinfo: inspect Havok packfiles (.hkx) from loose files or Skyrim BSAs.
//
//   hkxinfo <file.hkx> [mode...]
//   hkxinfo --data <dir> <internal/path.hkx> [mode...]
//
// Modes: --sections (default), --objects, --classes, --extract <out.hkx>,
//        --hex <offset> [count]
//
// Data-dependent (real Skyrim archives); not run in CI.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "bethesda/hkx.h"

namespace {

using rec::bethesda::HkxFile;

std::vector<rec::u8> ReadFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<rec::u8>((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
}

void PrintObjects(const HkxFile& hkx) {
  std::printf("%zu objects:\n", hkx.objects().size());
  for (const auto& obj : hkx.objects()) {
    std::printf("  %08llx %.*s\n", static_cast<unsigned long long>(obj.offset),
                static_cast<int>(obj.class_name.size()), obj.class_name.data());
  }
}

void PrintClasses(const HkxFile& hkx) {
  std::map<std::string, int> histogram;
  for (const auto& obj : hkx.objects()) histogram[std::string(obj.class_name)]++;
  for (const auto& [name, count] : histogram) {
    std::printf("  %4d %s\n", count, name.c_str());
  }
}

void PrintHex(const HkxFile& hkx, rec::u64 offset, rec::u64 count) {
  for (rec::u64 row = 0; row < count; row += 16) {
    std::printf("%08llx ", static_cast<unsigned long long>(offset + row));
    for (rec::u64 i = 0; i < 16 && offset + row + i < hkx.data_size(); ++i) {
      std::printf("%02x%s", hkx.data()[offset + row + i], (i % 4 == 3) ? " " : "");
    }
    // Pointer / float annotations per 8 bytes.
    std::printf(" |");
    for (rec::u64 i = 0; i < 16; i += 8) {
      rec::u64 at = offset + row + i;
      rec::u64 target = hkx.Pointer(at);
      if (target != HkxFile::kNull) {
        std::string_view cls = hkx.class_of(target);
        std::printf(" ->%llx%s%.*s", static_cast<unsigned long long>(target),
                    cls.empty() ? "" : ":", static_cast<int>(cls.size()), cls.data());
      }
    }
    std::printf(" | %g %g %g %g\n", hkx.F32(offset + row), hkx.F32(offset + row + 4),
                hkx.F32(offset + row + 8), hkx.F32(offset + row + 12));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) {
    std::fprintf(stderr, "usage: hkxinfo <file.hkx> | --data <dir> <internal/path> [modes]\n");
    return 1;
  }

  std::vector<rec::u8> bytes;
  size_t consumed = 0;
  if (args[0] == "--data") {
    if (args.size() < 3) {
      std::fprintf(stderr, "--data needs <dir> <internal/path>\n");
      return 1;
    }
    rec::asset::Vfs vfs;
    for (const auto& entry : std::filesystem::directory_iterator(args[1])) {
      if (entry.path().extension() == ".bsa") {
        if (auto provider = rec::bethesda::OpenArchive(entry.path().string())) {
          vfs.Mount(std::move(provider));
        }
      }
    }
    std::printf("mounted %zu archives\n", vfs.mount_count());
    auto data = vfs.Read(args[2]);
    if (!data) {
      std::fprintf(stderr, "not found in archives: %s\n", args[2].c_str());
      return 1;
    }
    bytes.assign(data->begin(), data->end());
    consumed = 3;
  } else {
    bytes = ReadFileBytes(args[0]);
    if (bytes.empty()) {
      std::fprintf(stderr, "cannot read %s\n", args[0].c_str());
      return 1;
    }
    consumed = 1;
  }

  auto hkx = HkxFile::Parse(bytes.data(), bytes.size());
  if (!hkx) {
    std::fprintf(stderr, "not a supported havok packfile (%zu bytes, magic %02x%02x%02x%02x)\n",
                 bytes.size(), bytes.size() > 0 ? bytes[0] : 0, bytes.size() > 1 ? bytes[1] : 0,
                 bytes.size() > 2 ? bytes[2] : 0, bytes.size() > 3 ? bytes[3] : 0);
    return 1;
  }
  std::printf("havok packfile: %s, %u-bit pointers, data %zu bytes, %zu objects\n",
              hkx->content_version().c_str(), hkx->pointer_size() * 8, hkx->data_size(),
              hkx->objects().size());

  bool any_mode = false;
  for (size_t i = consumed; i < args.size(); ++i) {
    any_mode = true;
    if (args[i] == "--objects") {
      PrintObjects(*hkx);
    } else if (args[i] == "--classes") {
      PrintClasses(*hkx);
    } else if (args[i] == "--extract" && i + 1 < args.size()) {
      std::ofstream out(args[++i], std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
      std::printf("wrote %s\n", args[i].c_str());
    } else if (args[i] == "--hex" && i + 1 < args.size()) {
      rec::u64 offset = std::strtoull(args[++i].c_str(), nullptr, 0);
      rec::u64 count = 128;
      if (i + 1 < args.size() && args[i + 1][0] != '-') {
        count = std::strtoull(args[++i].c_str(), nullptr, 0);
      }
      PrintHex(*hkx, offset, count);
    } else if (args[i] == "--sections") {
      // Header line above already covers the summary.
    } else {
      std::fprintf(stderr, "unknown mode %s\n", args[i].c_str());
    }
  }
  if (!any_mode) PrintClasses(*hkx);
  return 0;
}
