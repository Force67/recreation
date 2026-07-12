#include "bethesda/biom.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace rx::bethesda {
namespace {

// Little cursor over the blob. Every read bounds-checks; the first failure
// leaves the map invalid.
struct Reader {
  const u8* p;
  const u8* end;
  bool ok = true;

  bool Need(size_t n) {
    if (!ok || static_cast<size_t>(end - p) < n) ok = false;
    return ok;
  }
  u16 U16() {
    if (!Need(2)) return 0;
    u16 v;
    std::memcpy(&v, p, 2);
    p += 2;
    return v;
  }
  u32 U32() {
    if (!Need(4)) return 0;
    u32 v;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
  }
  // Copies `count` u32 into `out`.
  void U32Array(base::Vector<u32>* out, u32 count) {
    if (!Need(static_cast<size_t>(count) * 4)) return;
    out->resize(count);
    std::memcpy(out->data(), p, static_cast<size_t>(count) * 4);
    p += static_cast<size_t>(count) * 4;
  }
  void U8Array(base::Vector<u8>* out, u32 count) {
    if (!Need(count)) return;
    out->resize(count);
    std::memcpy(out->data(), p, count);
    p += count;
  }
};

// Reads one hemisphere. `has_num_grids` is set for the first region, whose
// header carries a leading numGrids the second region omits.
bool ReadHemisphere(Reader& r, BiomeMap::Hemisphere* hemi, bool has_num_grids) {
  if (has_num_grids) r.U32();  // numGrids (2 total, both regions counted here)
  const u32 w = r.U32();
  const u32 h = r.U32();
  const u32 n = r.U32();
  if (!r.ok || w != BiomeMap::kDim || h != BiomeMap::kDim || n != BiomeMap::kCells) return false;
  r.U32Array(&hemi->biome, n);
  const u32 flat = r.U32();
  if (!r.ok || flat != n) return false;
  r.U8Array(&hemi->resource, n);
  return r.ok;
}

}  // namespace

u32 BiomeMap::BiomeAt(u32 hemisphere, u32 x, u32 y) const {
  if (hemisphere > 1) hemisphere = 0;
  const Hemisphere& hemi = hemispheres[hemisphere];
  if (hemi.biome.size() != kCells) return 0;
  x = std::min(x, kDim - 1);
  y = std::min(y, kDim - 1);
  return hemi.biome[y * kDim + x];
}

u32 BiomeMap::DominantBiome() const {
  const Hemisphere& hemi = hemispheres[0];
  if (hemi.biome.size() != kCells) return 0;
  std::unordered_map<u32, u32> counts;
  for (u32 id : hemi.biome) ++counts[id];
  u32 best = 0, best_count = 0;
  for (const auto& [id, count] : counts) {
    if (count > best_count) {
      best = id;
      best_count = count;
    }
  }
  return best;
}

BiomeMap ParseBiomeMap(ByteSpan data) {
  BiomeMap map;
  Reader r{data.data(), data.data() + data.size()};

  const u16 magic = r.U16();
  if (!r.ok || magic != 0x0105) return map;
  const u32 num_biomes = r.U32();
  if (!r.ok || num_biomes == 0 || num_biomes > 64) return map;
  r.U32Array(&map.biome_ids, num_biomes);
  if (!r.ok) return map;

  if (!ReadHemisphere(r, &map.hemispheres[0], /*has_num_grids=*/true)) return map;
  if (!ReadHemisphere(r, &map.hemispheres[1], /*has_num_grids=*/false)) return map;

  map.valid = true;
  return map;
}

}  // namespace rx::bethesda
