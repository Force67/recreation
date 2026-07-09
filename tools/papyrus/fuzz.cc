// pexfuzz: feed malformed Papyrus containers to ParsePex and assert it never
// crashes, only rejects. The VM loads mod-provided .pex, so the parser must be
// robust against truncated and corrupted input, not just well-formed shipped
// files. Deterministic (no RNG): every truncation length plus strided single-
// byte corruptions of real scripts.
//
//   pexfuzz <data_dir>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "script/papyrus/pex.h"

using namespace rx;
using namespace rx::script::papyrus;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <data_dir>\n", argv[0]);
    return 2;
  }
  asset::Vfs vfs;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(argv[1], ec))
    if (auto p = bethesda::OpenArchive(e.path().string())) vfs.Mount(std::move(p));

  // A spread of real scripts: tiny, medium, and a large one with deep tables.
  const char* names[] = {"scripts/trapbase.pex", "scripts/quest.pex", "scripts/actor.pex",
                        "scripts/objectreference.pex"};
  long attempts = 0, accepted = 0;
  for (const char* name : names) {
    auto blob = vfs.Read(name);
    if (!blob) {
      std::printf("skip %s (not found)\n", name);
      continue;
    }
    std::vector<u8> full(blob->begin(), blob->end());

    // Truncation: every prefix length must be rejected or parsed, never crash.
    for (size_t len = 0; len <= full.size(); ++len) {
      PexFile pex;
      if (ParsePex(ByteSpan(full.data(), len), &pex)) ++accepted;
      ++attempts;
    }

    // Single-byte corruption at strided offsets, several bit patterns each.
    const u8 masks[] = {0xFF, 0x01, 0x80, 0x7F};
    size_t step = full.size() > 4096 ? full.size() / 4096 : 1;
    for (size_t off = 0; off < full.size(); off += step) {
      u8 original = full[off];
      for (u8 m : masks) {
        full[off] = static_cast<u8>(original ^ m);
        PexFile pex;
        if (ParsePex(ByteSpan(full.data(), full.size()), &pex)) ++accepted;
        ++attempts;
      }
      full[off] = original;
    }
    std::printf("fuzzed %s (%zu bytes)\n", name, blob->size());
  }

  std::printf("survived %ld malformed-input parses (%ld accepted), no crash\n", attempts, accepted);
  return 0;
}
