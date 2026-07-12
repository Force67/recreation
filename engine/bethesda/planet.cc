#include "bethesda/planet.h"

#include <algorithm>
#include <cstring>

#include "bethesda/record.h"
#include "core/log.h"

namespace rx::bethesda {
namespace {

constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kSnam = FourCc('S', 'N', 'A', 'M');
constexpr u32 kLnam = FourCc('L', 'N', 'A', 'M');
constexpr u32 kBnam = FourCc('B', 'N', 'A', 'M');
constexpr u32 kBmc1 = FourCc('B', 'M', 'C', '1');

// The BIOM LNAM material-layer block carries the referenced LTEX FormID at
// offset 4 (a leading u32 index precedes it). Zero when absent/short.
u32 LnamLtexRef(const Subrecord& lnam) {
  if (lnam.data.size() < 8) return 0;
  u32 raw;
  std::memcpy(&raw, lnam.data.data() + 4, 4);
  return raw;
}

// BMC1 is a packed RGBA8 map colour. Returns linear-ish 0..1 rgb (a plain /255,
// good enough as a fallback ground tint).
void ReadTint(const Record& biom, f32 tint[3]) {
  const Subrecord* bmc = biom.Find(kBmc1);
  if (!bmc || bmc->data.size() < 4) return;
  const u8* c = bmc->data.data();
  tint[0] = c[0] / 255.0f;
  tint[1] = c[1] / 255.0f;
  tint[2] = c[2] / 255.0f;
}

}  // namespace

BiomeGround ResolveBiomeGround(const RecordStore& records, const StarfieldMaterialDb& mat_db,
                               u32 raw_biome_id, u16 biom_plugin) {
  BiomeGround out;
  out.form_id = raw_biome_id;

  const GlobalFormId biome_id = records.ResolveFrom(RawFormId{raw_biome_id}, biom_plugin);
  Record biom;
  if (biome_id.plugin == 0xffff || !records.Parse(biome_id, &biom)) return out;

  out.editor_id = biom.GetString(kEdid);
  out.surface = biom.GetString(kSnam);
  ReadTint(biom, out.tint);

  // First LNAM -> LTEX -> its BNAM .mat -> textures.
  u16 biome_owner = records.Find(biome_id) ? records.Find(biome_id)->winning_plugin : biom_plugin;
  for (const Subrecord& sub : biom.subrecords) {
    if (sub.type != kLnam) continue;
    u32 ltex_raw = LnamLtexRef(sub);
    if (ltex_raw == 0) continue;
    const GlobalFormId ltex_id = records.ResolveFrom(RawFormId{ltex_raw}, biome_owner);
    Record ltex;
    if (ltex_id.plugin == 0xffff || !records.Parse(ltex_id, &ltex)) continue;
    std::string mat = ltex.GetString(kBnam);  // "Materials\Terrain\...mat"
    if (mat.empty()) continue;
    out.ground_mat = mat;
    StarfieldMaterialDb::Resolved r;
    if (mat_db.Lookup(mat, &r)) {
      out.base_color = r.base_color;
      out.normal = r.normal;
    }
    break;  // first resolvable layer is enough for the MVP ground
  }

  out.valid = true;
  return out;
}

PlanetSurface LoadPlanetSurface(const asset::Vfs& vfs, const RecordStore& records,
                                const StarfieldMaterialDb& mat_db, const std::string& biom_name,
                                u16 biom_plugin) {
  PlanetSurface surface;
  surface.name = biom_name;

  // The Vfs normalizes case/slashes, so a plain lowercase path resolves the base
  // game files; a couple of DLC subdirs are tried as a courtesy.
  const std::string candidates[] = {
      "planetdata/biomemaps/" + biom_name + ".biom",
      "planetdata/biomemaps/shatteredspace.esm/" + biom_name + ".biom",
      "planetdata/biomemaps/dlc001/" + biom_name + ".biom",
  };
  std::optional<base::Vector<u8>> bytes;
  for (const std::string& path : candidates) {
    bytes = vfs.Read(path);
    if (bytes) break;
  }
  if (!bytes) {
    RX_WARN("planet: no .biom for '{}'", biom_name);
    return surface;
  }

  surface.map = ParseBiomeMap(ByteSpan(bytes->data(), bytes->size()));
  if (!surface.map.valid) {
    RX_WARN("planet: malformed .biom for '{}'", biom_name);
    return surface;
  }

  for (u32 id : surface.map.biome_ids)
    surface.grounds.push_back(ResolveBiomeGround(records, mat_db, id, biom_plugin));

  RX_INFO("planet '{}': {} biomes, dominant {:#x}", biom_name, surface.map.biome_ids.size(),
          surface.map.DominantBiome());
  for (const BiomeGround& g : surface.grounds)
    RX_INFO("  biome {:#x} {} surface={} mat={} tex={}", g.form_id, g.editor_id, g.surface,
            g.ground_mat, g.base_color.empty() ? "(none)" : g.base_color);

  surface.valid = true;
  return surface;
}

}  // namespace rx::bethesda
