#ifndef RECREATION_BETHESDA_PLANET_H_
#define RECREATION_BETHESDA_PLANET_H_

#include <string>

#include "asset/vfs.h"
#include "bethesda/biom.h"
#include "bethesda/form_id.h"
#include "bethesda/load_order.h"
#include "bethesda/material_db.h"

namespace rx::bethesda {

// The minimal biome-archetype data the procedural-tile generator needs to pick
// a ground surface: resolved from a BIOM record (its terrain material layers
// LNAM -> LTEX -> .mat, and the map colour BMC as a fallback tint).
struct BiomeGround {
  u32 form_id = 0;            // raw BIOM FormID
  std::string editor_id;     // e.g. "CrateredNoLife09"
  std::string surface;       // SNAM archetype, e.g. "Cratered"
  std::string ground_mat;    // first LTEX's .mat path (Materials\Terrain\...mat)
  std::string base_color;    // resolved diffuse texture ("textures/...dds"), may be empty
  std::string normal;        // resolved normal texture, may be empty
  f32 tint[3] = {0.5f, 0.5f, 0.5f};  // BMC map colour, linear 0..1 (fallback ground colour)
  bool valid = false;
};

// Resolves a raw BIOM FormID (from a .biom cell, owned by `biom_plugin`) into the
// ground data above, following LNAM -> LTEX -> .mat and, when `mat_db` is
// non-empty, .mat -> texture paths. `mat_db` may be empty (base_color/normal stay
// blank and the caller falls back to the tint).
BiomeGround ResolveBiomeGround(const RecordStore& records, const StarfieldMaterialDb& mat_db,
                               u32 raw_biome_id, u16 biom_plugin);

// A resolved procedural-planet target: its biome map plus per-biome ground data,
// ready to drive tile generation. Loaded from planetdata/biomemaps/<name>.biom.
struct PlanetSurface {
  std::string name;   // the .biom stem passed in (e.g. "zeta ophiuchi ii")
  BiomeMap map;       // decoded .biom
  // Ground data for each biome id in map.biome_ids, same order.
  base::Vector<BiomeGround> grounds;
  bool valid = false;
};

// Loads and resolves a planet surface. `biom_name` is the .biom stem WITHOUT the
// extension (case-insensitive path lookup handled by the Vfs), e.g.
// "zeta ophiuchi ii" or "zeta ophiuchi vi-e". Tries the plain path first, then a
// few known DLC subdirectories. Returns valid=false when the file is missing or
// malformed.
PlanetSurface LoadPlanetSurface(const asset::Vfs& vfs, const RecordStore& records,
                                const StarfieldMaterialDb& mat_db, const std::string& biom_name,
                                u16 biom_plugin);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_PLANET_H_
