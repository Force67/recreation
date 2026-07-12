// biomtest: deterministic checks for the Starfield .biom biome-map decoder.
// Builds a byte-exact synthetic .biom (two 256x256 hemispheres, the first with a
// leading numGrids the second omits) and verifies parse, cell sampling, and the
// dominant-biome pick.

#include <cstdio>
#include <cstring>
#include <vector>

#include "bethesda/biom.h"
#include "core/types.h"

namespace {

using rx::u16;
using rx::u32;
using rx::u8;

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void PutU16(std::vector<u8>& b, u16 v) { b.insert(b.end(), {u8(v), u8(v >> 8)}); }
void PutU32(std::vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (8 * i)));
}

constexpr u32 kDim = 256, kCells = kDim * kDim;

// Fills a hemisphere's biome grid: cells 0..half are biome A, the rest biome B.
void PutHemisphere(std::vector<u8>& b, bool with_num_grids, u32 biome_a, u32 biome_b, u32 a_cells) {
  if (with_num_grids) PutU32(b, 2);  // numGrids (both regions counted)
  PutU32(b, kDim);
  PutU32(b, kDim);
  PutU32(b, kCells);
  for (u32 i = 0; i < kCells; ++i) PutU32(b, i < a_cells ? biome_a : biome_b);
  PutU32(b, kCells);                          // resource sub-header (flat)
  for (u32 i = 0; i < kCells; ++i) b.push_back(static_cast<u8>(i & 0x07));
}

}  // namespace

int main() {
  std::puts("biomtest");

  const u32 kBiomeA = 0x0c5719;  // CrateredNoLife09 in the real data
  const u32 kBiomeB = 0x08dffd;

  std::vector<u8> blob;
  PutU16(blob, 0x0105);  // magic
  PutU32(blob, 2);       // numBiomes
  PutU32(blob, kBiomeA);
  PutU32(blob, kBiomeB);
  // Hemisphere 0: 60000 cells biome A, rest biome B (A dominant).
  PutHemisphere(blob, /*with_num_grids=*/true, kBiomeA, kBiomeB, 60000);
  // Hemisphere 1: no leading numGrids, mostly biome B.
  PutHemisphere(blob, /*with_num_grids=*/false, kBiomeA, kBiomeB, 1000);

  rx::bethesda::BiomeMap map =
      rx::bethesda::ParseBiomeMap(rx::ByteSpan(blob.data(), blob.size()));

  Check("parses valid", map.valid);
  Check("two biome ids", map.biome_ids.size() == 2);
  Check("biome table entry 0", map.biome_ids.size() == 2 && map.biome_ids[0] == kBiomeA);
  Check("hemisphere 0 cell (0,0) is biome A", map.BiomeAt(0, 0, 0) == kBiomeA);
  // cell index 60000 = row 234, col 96 -> first biome-B cell.
  Check("hemisphere 0 late cell is biome B", map.BiomeAt(0, 255, 255) == kBiomeB);
  Check("dominant biome is A", map.DominantBiome() == kBiomeA);
  Check("hemisphere 1 stored", map.hemispheres[1].biome.size() == kCells);
  Check("hemisphere 1 mostly biome B", map.BiomeAt(1, 255, 255) == kBiomeB);
  Check("resource channel sized", map.hemispheres[0].resource.size() == kCells);

  // A truncated blob must not parse.
  std::vector<u8> truncated(blob.begin(), blob.begin() + 100);
  rx::bethesda::BiomeMap bad =
      rx::bethesda::ParseBiomeMap(rx::ByteSpan(truncated.data(), truncated.size()));
  Check("truncated blob rejected", !bad.valid);

  // Wrong magic rejected.
  std::vector<u8> wrong = blob;
  wrong[0] = 0xff;
  rx::bethesda::BiomeMap bad_magic =
      rx::bethesda::ParseBiomeMap(rx::ByteSpan(wrong.data(), wrong.size()));
  Check("wrong magic rejected", !bad_magic.valid);

  if (g_failures == 0) {
    std::puts("biom: all checks passed");
    return 0;
  }
  std::printf("biom: %d checks FAILED\n", g_failures);
  return 1;
}
