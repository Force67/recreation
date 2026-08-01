#ifndef RECREATION_BETHESDA_BIOM_H_
#define RECREATION_BETHESDA_BIOM_H_

#include <base/containers/vector.h>

#include "core/types.h"

namespace rx::bethesda {

// Starfield per-planet-face biome map (planetdata/biomemaps/<planet>-<face>.biom).
// A raster grid, NOT engine CELLs: the runtime samples it to pick which biome
// (its textures/scatter/flora) applies where on the globe. Terrain SHAPE is not
// in here (that is the separate .btd overlay system, which the procedural-tile
// generator sidesteps with engine-native noise).
//
// Format (verified byte-exact on real samples, magic 0x0105):
//   u16 magic (0x0105)
//   u32 numBiomes (1..8)
//   u32 biomeIds[numBiomes]           BIOM record FormIDs
//   region 0 (hemisphere A): u32 numGrids, u32 w=256, u32 h=256, u32 n=65536,
//                            u32 biome[n], u32 flat=n, u8 resource[n]
//   region 1 (hemisphere B): u32 w=256, u32 h=256, u32 n=65536,   (NO numGrids)
//                            u32 biome[n], u32 flat=n, u8 resource[n]
// biome[] cells are all members of biomeIds[]; resource[] is a distinct small
// channel (surface resource deposits, values like {0,1,2,3,8}).
struct BiomeMap {
  static constexpr u32 kDim = 256;
  static constexpr u32 kCells = kDim * kDim;

  base::Vector<u32> biome_ids;  // the file's biome table (raw FormIDs)

  struct Hemisphere {
    base::Vector<u32> biome;    // kCells raw BIOM FormIDs, one per grid cell
    base::Vector<u8> resource;  // kCells resource-overlay bytes
  };
  Hemisphere hemispheres[2];

  bool valid = false;

  // Raw BIOM FormID at grid cell (x,y) of a hemisphere (0 or 1), clamped.
  u32 BiomeAt(u32 hemisphere, u32 x, u32 y) const;
  // The dominant (most-covered) biome id across hemisphere 0, a good "landing
  // site" pick for a barren tile. Returns 0 when the map is empty.
  u32 DominantBiome() const;
};

// Parses a .biom blob. Returns a map with valid=false on any structural error.
BiomeMap ParseBiomeMap(ByteSpan data);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_BIOM_H_
