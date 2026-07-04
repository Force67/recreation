#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "bethesda/load_order.h"
#include "bethesda/plugin.h"

namespace {

using namespace rec::bethesda;

constexpr rec::u32 kEdid = rec::FourCc('E', 'D', 'I', 'D');
constexpr rec::u32 kName = rec::FourCc('N', 'A', 'M', 'E');
constexpr rec::u32 kData = rec::FourCc('D', 'A', 'T', 'A');
constexpr rec::u32 kModl = rec::FourCc('M', 'O', 'D', 'L');

// Lists the refs of one exterior cell with their base record info, for
// chasing down "what is that thing on screen" questions.
int DumpCellRefs(const std::string& data_dir, int x, int y) {
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) return 1;

  GlobalFormId world = records.FindWorldspace(profile.exterior_worldspace);
  const RecordStore::ExteriorGrid* grid = records.ExteriorCells(world);
  if (!grid) return 1;
  const RecordStore::ExteriorCell* cell =
      grid->find(RecordStore::GridKey(static_cast<rec::i16>(x), static_cast<rec::i16>(y)));
  if (!cell) {
    std::printf("no cell at %d,%d\n", x, y);
    return 1;
  }
  std::printf("cell %d,%d: %zu refs\n", x, y, static_cast<size_t>(cell->refs.size()));
  for (rec::u64 packed : cell->refs) {
    GlobalFormId id{static_cast<rec::u16>(packed >> 32), static_cast<rec::u32>(packed)};
    Record refr;
    if (!records.Parse(id, &refr)) continue;
    const Subrecord* name = refr.Find(kName);
    const Subrecord* data = refr.Find(kData);
    if (!name || name->data.size() < 4) continue;
    rec::u32 base_raw;
    std::memcpy(&base_raw, name->data.data(), 4);
    GlobalFormId base_id = records.ResolveFrom(RawFormId{base_raw}, records.Find(id)->winning_plugin);
    const RecordStore::StoredRecord* base_stored = records.Find(base_id);
    char type[5] = {};
    if (base_stored) std::memcpy(type, &base_stored->header.type, 4);
    Record base;
    std::string edid, model;
    if (base_stored && records.Parse(base_id, &base)) {
      edid = base.GetString(kEdid);
      model = base.GetString(kModl);
    }
    float pos[3] = {};
    if (data && data->data.size() >= 12) std::memcpy(pos, data->data.data(), 12);
    std::printf("  %04x:%06x %s %-32s (%.0f,%.0f,%.0f) %s\n", id.plugin, id.local_id, type,
                edid.c_str(), pos[0], pos[1], pos[2], model.c_str());
  }
  return 0;
}

// Dumps the LAND texture layers of one exterior cell, for grass placement
// work.
int DumpLand(const std::string& data_dir, int x, int y) {
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) return 1;

  GlobalFormId world = records.FindWorldspace(profile.exterior_worldspace);
  const RecordStore::ExteriorGrid* grid = records.ExteriorCells(world);
  const RecordStore::ExteriorCell* cell =
      grid ? grid->find(RecordStore::GridKey(static_cast<rec::i16>(x), static_cast<rec::i16>(y)))
           : nullptr;
  if (!cell || cell->land == 0) {
    std::printf("no land at %d,%d\n", x, y);
    return 1;
  }
  GlobalFormId land_id{static_cast<rec::u16>(cell->land >> 32),
                       static_cast<rec::u32>(cell->land)};
  Record land;
  if (!records.Parse(land_id, &land)) return 1;
  rec::u16 plugin = records.Find(land_id)->winning_plugin;

  auto ltex_name = [&](rec::u32 raw) {
    if (raw == 0) return std::string("(default)");
    GlobalFormId id = records.ResolveFrom(RawFormId{raw}, plugin);
    Record ltex;
    char buffer[64];
    std::string edid;
    if (records.Parse(id, &ltex)) edid = ltex.GetString(kEdid);
    std::snprintf(buffer, sizeof(buffer), "%04x:%06x %s", id.plugin, id.local_id, edid.c_str());
    return std::string(buffer);
  };

  constexpr rec::u32 kBtxt = rec::FourCc('B', 'T', 'X', 'T');
  constexpr rec::u32 kAtxt = rec::FourCc('A', 'T', 'X', 'T');
  constexpr rec::u32 kVtxt = rec::FourCc('V', 'T', 'X', 'T');
  for (const Subrecord& sub : land.subrecords) {
    if ((sub.type == kBtxt || sub.type == kAtxt) && sub.data.size() >= 8) {
      rec::u32 raw;
      std::memcpy(&raw, sub.data.data(), 4);
      std::printf("%s quadrant=%u %s\n", sub.type == kBtxt ? "BTXT" : "ATXT", sub.data[4],
                  ltex_name(raw).c_str());
    } else if (sub.type == kVtxt) {
      size_t count = sub.data.size() / 8;
      float sum = 0, max = 0;
      for (size_t i = 0; i + 8 <= sub.data.size(); i += 8) {
        float opacity;
        std::memcpy(&opacity, sub.data.data() + i + 4, 4);
        sum += opacity;
        if (opacity > max) max = opacity;
      }
      std::printf("  VTXT %zu points (of 289), mean=%.2f max=%.2f\n", count,
                  count ? sum / static_cast<float>(count) : 0.0f, max);
    }
  }
  return 0;
}

// Dumps every GRAS record and the LTEX -> GRAS links, for grass placement
// work.
int DumpGrass(const std::string& data_dir) {
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) return 1;

  constexpr rec::u32 kGnam = rec::FourCc('G', 'N', 'A', 'M');
  records.EachOfType(rec::FourCc('G', 'R', 'A', 'S'),
                     [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    Record gras;
    if (!records.Parse(id, &gras)) return;
    const Subrecord* data = gras.Find(kData);
    std::printf("GRAS %04x:%06x %-24s model=%s data=%zu\n", id.plugin, id.local_id,
                gras.GetString(kEdid).c_str(), gras.GetString(kModl).c_str(),
                data ? static_cast<size_t>(data->data.size()) : 0);
    if (data && data->data.size() >= 32) {
      const rec::u8* d = data->data.data();
      rec::u16 units_from_water;
      rec::u32 water_type;
      float pos_range, height_range, color_range, wave_period;
      std::memcpy(&units_from_water, d + 4, 2);
      std::memcpy(&water_type, d + 8, 4);
      std::memcpy(&pos_range, d + 12, 4);
      std::memcpy(&height_range, d + 16, 4);
      std::memcpy(&color_range, d + 20, 4);
      std::memcpy(&wave_period, d + 24, 4);
      std::printf("  density=%u slope=%u..%u water=%u type=%u posR=%.1f heightR=%.1f "
                  "colorR=%.2f wave=%.2f flags=%02x\n",
                  d[0], d[1], d[2], units_from_water, water_type, pos_range, height_range,
                  color_range, wave_period, d[28]);
    }
  });

  records.EachOfType(rec::FourCc('L', 'T', 'E', 'X'),
                     [&](GlobalFormId id, const RecordStore::StoredRecord& stored) {
    Record ltex;
    if (!records.Parse(id, &ltex)) return;
    std::string grass;
    for (const Subrecord& sub : ltex.subrecords) {
      if (sub.type != kGnam || sub.data.size() < 4) continue;
      rec::u32 raw;
      std::memcpy(&raw, sub.data.data(), 4);
      GlobalFormId gras_id = records.ResolveFrom(RawFormId{raw}, stored.winning_plugin);
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), " %04x:%06x", gras_id.plugin, gras_id.local_id);
      grass += buffer;
    }
    if (!grass.empty()) {
      std::printf("LTEX %04x:%06x %-24s grass:%s\n", id.plugin, id.local_id,
                  ltex.GetString(kEdid).c_str(), grass.c_str());
    }
  });
  return 0;
}

// Dumps each WTHR record's subrecords (type + size), and decodes the NAM0
// weather-colour block as RGBA quads, for wiring authored weather colours into
// the physical sky. Run on a data dir: esminfo <data-dir> wthr [limit].
int DumpWeather(const std::string& data_dir, int limit) {
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) return 1;

  constexpr rec::u32 kNam0 = rec::FourCc('N', 'A', 'M', '0');
  int shown = 0;
  records.EachOfType(rec::FourCc('W', 'T', 'H', 'R'),
                     [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    if (limit > 0 && shown >= limit) return;
    Record w;
    if (!records.Parse(id, &w)) return;
    ++shown;
    std::printf("WTHR %04x:%06x %s\n", id.plugin, id.local_id, w.GetString(kEdid).c_str());
    for (const Subrecord& sub : w.subrecords) {
      char t[5] = {};
      std::memcpy(t, &sub.type, 4);
      std::printf("  %s  %zu\n", t, static_cast<size_t>(sub.data.size()));
    }
    constexpr rec::u32 kDalc = rec::FourCc('D', 'A', 'L', 'C');
    int dalc_i = 0;
    for (const Subrecord& sub : w.subrecords) {
      if (sub.type != kDalc || sub.data.size() < 24) continue;
      const rec::u8* d = sub.data.data();
      std::printf("  DALC[%d]:", dalc_i++);
      for (int k = 0; k < 6; ++k)  // X+ X- Y+ Y- Z+ Z- hemisphere ambient (RGBA)
        std::printf(" %3u/%3u/%3u", d[k * 4], d[k * 4 + 1], d[k * 4 + 2]);
      std::printf("\n");
    }
    if (const Subrecord* nam0 = w.Find(kNam0); nam0 && nam0->data.size() % 16 == 0) {
      const rec::u8* d = nam0->data.data();
      size_t quads = nam0->data.size() / 4;  // RGBA bytes; 4 times-of-day per type
      std::printf("  NAM0 decoded (%zu colours, 4 per type = dawn/day/dusk/night):\n", quads);
      for (size_t i = 0; i < quads; ++i)
        std::printf("    [%2zu] %3u %3u %3u %3u%s", i, d[i * 4 + 0], d[i * 4 + 1], d[i * 4 + 2],
                    d[i * 4 + 3], (i % 4 == 3) ? "\n" : "  ");
    }
  });
  return 0;
}

// Decodes each WATR record's DNAM colour block (Skyrim SE, 228 bytes) as the
// Shallow/Deep/Reflection RGBA quads at offsets 40/44/48, plus the above-water
// fog near/far/amount floats, to verify the offsets against real data.
// esminfo <data-dir> watr [limit].
int DumpWater(const std::string& data_dir, int limit) {
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) return 1;

  constexpr rec::u32 kDnam = rec::FourCc('D', 'N', 'A', 'M');
  int shown = 0, total = 0;
  records.EachOfType(rec::FourCc('W', 'A', 'T', 'R'),
                     [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    ++total;
    if (limit > 0 && shown >= limit) return;
    Record w;
    if (!records.Parse(id, &w)) return;
    const Subrecord* dnam = w.Find(kDnam);
    if (!dnam || dnam->data.size() < 52) return;
    ++shown;
    const rec::u8* d = dnam->data.data();
    float fog_near, fog_far, fog_amount = 0;
    std::memcpy(&fog_near, d + 32, 4);
    std::memcpy(&fog_far, d + 36, 4);
    if (dnam->data.size() >= 136) std::memcpy(&fog_amount, d + 132, 4);
    std::printf("WATR %04x:%06x %-28s dnam=%zu  shallow=%3u,%3u,%3u deep=%3u,%3u,%3u "
                "refl=%3u,%3u,%3u  fog near=%.0f far=%.0f amt=%.2f\n",
                id.plugin, id.local_id, w.GetString(kEdid).c_str(),
                static_cast<size_t>(dnam->data.size()), d[40], d[41], d[42], d[44], d[45], d[46],
                d[48], d[49], d[50], fog_near, fog_far, fog_amount);
  });
  std::printf("total WATR: %d\n", total);
  return 0;
}

// Lists exterior cells of the primary worldspace whose XCWT water type differs
// from the worldspace NAM2 default, with their grid coords + water editor id,
// for finding a positive test pose (marsh/blood/spring water bodies).
// esminfo <data-dir> cellwater [limit].
int DumpCellWater(const std::string& data_dir, int limit) {
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) return 1;

  constexpr rec::u32 kXcwt = rec::FourCc('X', 'C', 'W', 'T');
  constexpr rec::u32 kNam2 = rec::FourCc('N', 'A', 'M', '2');
  GlobalFormId world = records.FindWorldspace(profile.exterior_worldspace);
  const RecordStore::ExteriorGrid* grid = records.ExteriorCells(world);
  if (!grid) return 1;

  GlobalFormId default_water{0xffff, 0};
  Record wrld;
  if (records.Parse(world, &wrld)) {
    if (const Subrecord* nam2 = wrld.Find(kNam2); nam2 && nam2->data.size() >= 4) {
      rec::u32 raw;
      std::memcpy(&raw, nam2->data.data(), 4);
      default_water = records.ResolveFrom(RawFormId{raw}, records.Find(world)->winning_plugin);
    }
  }
  std::printf("worldspace default water %04x:%06x\n", default_water.plugin, default_water.local_id);

  int shown = 0;
  for (auto kv : *grid) {
    if (limit > 0 && shown >= limit) break;
    rec::i16 x = static_cast<rec::i16>(kv.key >> 16);
    rec::i16 y = static_cast<rec::i16>(kv.key & 0xffff);
    if (kv.value.cell == 0) continue;
    GlobalFormId cell_id{static_cast<rec::u16>(kv.value.cell >> 32),
                         static_cast<rec::u32>(kv.value.cell)};
    Record cell;
    if (!records.Parse(cell_id, &cell)) continue;
    const Subrecord* xcwt = cell.Find(kXcwt);
    if (!xcwt || xcwt->data.size() < 4) continue;
    rec::u32 raw;
    std::memcpy(&raw, xcwt->data.data(), 4);
    if (raw == 0) continue;
    GlobalFormId water = records.ResolveFrom(RawFormId{raw}, records.Find(cell_id)->winning_plugin);
    if (water == default_water) continue;
    Record watr;
    std::string edid = records.Parse(water, &watr) ? watr.GetString(kEdid) : std::string();
    std::printf("cell %4d,%4d  water %04x:%06x %s\n", x, y, water.plugin, water.local_id,
                edid.c_str());
    ++shown;
  }
  return 0;
}

// Counts REFRs whose base form is a TXST (placed decal projectors) across the
// load order, splits them into primary-worldspace exterior vs elsewhere, and
// dumps a sample's full subrecord list (XPRM box? XSCL? rotation?) plus the
// base TXST's diffuse/normal paths. esminfo <data-dir> txstrefs [limit].
int DumpTxstRefs(const std::string& data_dir, int limit) {
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) return 1;

  std::set<rec::u64> txst_ids;
  records.EachOfType(rec::FourCc('T', 'X', 'S', 'T'),
                     [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    txst_ids.insert(id.packed());
  });
  std::printf("TXST records: %zu\n", txst_ids.size());

  // Refs of the primary worldspace's exterior grid, to split the counts and
  // report which cells decals cluster in (screenshot poses).
  std::map<rec::u64, rec::u32> exterior_refs;  // ref -> grid key
  GlobalFormId world = records.FindWorldspace(profile.exterior_worldspace);
  if (const RecordStore::ExteriorGrid* grid = records.ExteriorCells(world)) {
    for (auto kv : *grid)
      for (rec::u64 packed : kv.value.refs) exterior_refs.emplace(packed, kv.key);
  }
  std::map<rec::u32, int> per_cell;

  constexpr rec::u32 kXprm = rec::FourCc('X', 'P', 'R', 'M');
  constexpr rec::u32 kXscl = rec::FourCc('X', 'S', 'C', 'L');
  constexpr rec::u32 kTx00 = rec::FourCc('T', 'X', '0', '0');
  constexpr rec::u32 kTx01 = rec::FourCc('T', 'X', '0', '1');
  int refr_total = 0, total = 0, exterior = 0, with_xprm = 0, shown = 0;
  std::map<rec::u64, int> per_base;
  records.EachOfType(rec::FourCc('R', 'E', 'F', 'R'),
                     [&](GlobalFormId id, const RecordStore::StoredRecord& stored) {
    ++refr_total;
    Record refr;
    if (!records.Parse(id, &refr)) return;
    const Subrecord* name = refr.Find(kName);
    if (!name || name->data.size() < 4) return;
    rec::u32 base_raw;
    std::memcpy(&base_raw, name->data.data(), 4);
    GlobalFormId base_id = records.ResolveFrom(RawFormId{base_raw}, stored.winning_plugin);
    if (!txst_ids.count(base_id.packed())) return;
    ++total;
    ++per_base[base_id.packed()];
    auto ext = exterior_refs.find(id.packed());
    bool in_exterior = ext != exterior_refs.end();
    if (in_exterior) {
      ++exterior;
      ++per_cell[ext->second];
    }
    if (refr.Find(kXprm)) ++with_xprm;
    if (limit > 0 && shown >= limit) return;
    ++shown;

    Record base;
    std::string edid, tx00, tx01;
    if (records.Parse(base_id, &base)) {
      edid = base.GetString(kEdid);
      tx00 = base.GetString(kTx00);
      tx01 = base.GetString(kTx01);
    }
    float placement[6] = {};
    if (const Subrecord* data = refr.Find(kData); data && data->data.size() >= 24)
      std::memcpy(placement, data->data.data(), 24);
    float scale = 1.0f;
    if (const Subrecord* xscl = refr.Find(kXscl); xscl && xscl->data.size() >= 4)
      std::memcpy(&scale, xscl->data.data(), 4);
    std::printf("REFR %04x:%06x %s base %04x:%06x %-28s pos (%.0f,%.0f,%.0f) rot "
                "(%.2f,%.2f,%.2f) scale %.2f\n",
                id.plugin, id.local_id, in_exterior ? "ext" : "int", base_id.plugin,
                base_id.local_id, edid.c_str(), placement[0], placement[1], placement[2],
                placement[3], placement[4], placement[5], scale);
    for (const Subrecord& sub : refr.subrecords) {
      char t[5] = {};
      std::memcpy(t, &sub.type, 4);
      std::printf("  %s  %4zu ", t, static_cast<size_t>(sub.data.size()));
      for (size_t i = 0; i + 4 <= sub.data.size() && i < 32; i += 4) {
        float f;
        std::memcpy(&f, sub.data.data() + i, 4);
        std::printf(" %.4g", f);
      }
      std::printf("\n");
    }
    if (!tx00.empty()) std::printf("  TX00 %s\n", tx00.c_str());
    if (!tx01.empty()) std::printf("  TX01 %s\n", tx01.c_str());
  });
  std::printf("REFRs scanned: %d\n", refr_total);
  std::printf("TXST-based REFRs: %d (exterior %s: %d, elsewhere: %d, with XPRM: %d, distinct "
              "bases: %zu)\n",
              total, profile.exterior_worldspace.c_str(), exterior, total - exterior, with_xprm,
              per_base.size());

  // Placed bases with their DODT decal data (widths/heights/depth in game
  // units), the numbers the engine needs for projection box extents.
  constexpr rec::u32 kDodt = rec::FourCc('D', 'O', 'D', 'T');
  for (const auto& [packed, count] : per_base) {
    GlobalFormId base_id{static_cast<rec::u16>(packed >> 32), static_cast<rec::u32>(packed)};
    Record base;
    if (!records.Parse(base_id, &base)) continue;
    std::printf("base %04x:%06x x%-4d %-32s", base_id.plugin, base_id.local_id, count,
                base.GetString(kEdid).c_str());
    if (const Subrecord* dodt = base.Find(kDodt); dodt && dodt->data.size() >= 36) {
      float v[5];
      std::memcpy(v, dodt->data.data(), 20);
      const rec::u8* d = dodt->data.data();
      std::printf(" DODT w %.0f..%.0f h %.0f..%.0f depth %.0f flags %02x color %u,%u,%u",
                  v[0], v[1], v[2], v[3], v[4], d[29], d[32], d[33], d[34]);
    } else {
      std::printf(" (no DODT)");
    }
    std::printf(" tx00=%s\n", base.GetString(kTx00).c_str());
  }

  // Densest exterior cells, for finding a screenshot pose.
  std::multimap<int, rec::u32, std::greater<int>> dense;
  for (const auto& [key, count] : per_cell) dense.emplace(count, key);
  int listed = 0;
  for (const auto& [count, key] : dense) {
    if (++listed > 12) break;
    std::printf("cell %d,%d: %d decals\n", static_cast<rec::i16>(key >> 16),
                static_cast<rec::i16>(key & 0xffff), count);
  }
  return 0;
}

// Decodes the interior-cell lighting subrecords XCLL (per-cell lighting) and
// LTMP (lighting-template ref), for wiring authored interior ambience. Layout
// per xEdit wbDefinitionsTES5.pas: XCLL 'Lighting' = ambient/directional/fog-near
// byte colours, fog near/far floats, directional rotation xy/z (s32), directional
// fade, fog clip, fog power, then the 32-byte directional-ambient (DALC) block,
// fog-far colour, fog max, light fade begin/end, and a u32 Inherits flag field
// selecting per-group between the cell and its template. A non-numeric second
// arg filters by editor-id substring. esminfo <data-dir> xcll [limit|name].
int DumpInteriorLighting(const std::string& data_dir, int limit, const std::string& name_filter) {
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) return 1;

  constexpr rec::u32 kXcll = rec::FourCc('X', 'C', 'L', 'L');
  constexpr rec::u32 kLtmp = rec::FourCc('L', 'T', 'M', 'P');
  auto col = [](const rec::u8* d) {
    return std::string(std::to_string(d[0]) + "," + std::to_string(d[1]) + "," +
                       std::to_string(d[2]));
  };
  int shown = 0, interiors = 0, with_xcll = 0;
  records.EachOfType(rec::FourCc('C', 'E', 'L', 'L'),
                     [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    Record cell;
    if (!records.Parse(id, &cell)) return;
    const Subrecord* data = cell.Find(kData);
    if (!data || data->data.empty() || !(data->data[0] & 0x01)) return;  // interior flag
    ++interiors;
    std::string edid = cell.GetString(kEdid);
    if (!name_filter.empty() && edid.find(name_filter) == std::string::npos) return;
    const Subrecord* xcll = cell.Find(kXcll);
    const Subrecord* ltmp = cell.Find(kLtmp);
    if (!xcll && !ltmp) return;
    if (xcll) ++with_xcll;
    if (limit > 0 && shown >= limit) return;
    ++shown;

    std::printf("CELL %04x:%06x %-28s", id.plugin, id.local_id, edid.c_str());
    if (ltmp && ltmp->data.size() >= 4) {
      rec::u32 raw;
      std::memcpy(&raw, ltmp->data.data(), 4);
      GlobalFormId t = records.ResolveFrom(RawFormId{raw}, records.Find(id)->winning_plugin);
      Record lgtm;
      std::string ledid = records.Parse(t, &lgtm) ? lgtm.GetString(kEdid) : std::string();
      std::printf(" LTMP %04x:%06x %s", t.plugin, t.local_id, ledid.c_str());
    }
    std::printf("\n");
    if (!xcll) {
      std::printf("  (no XCLL, fully inherits template)\n");
      return;
    }
    const rec::u8* d = xcll->data.data();
    const size_t n = xcll->data.size();
    std::printf("  XCLL %zu  ambient=%s directional=%s fogNear=%s", n, col(d).c_str(),
                col(d + 4).c_str(), col(d + 8).c_str());
    if (n >= 40) {
      float fog_near, fog_far, dir_fade, fog_clip, fog_pow;
      rec::i32 rot_xy, rot_z;
      std::memcpy(&fog_near, d + 12, 4);
      std::memcpy(&fog_far, d + 16, 4);
      std::memcpy(&rot_xy, d + 20, 4);
      std::memcpy(&rot_z, d + 24, 4);
      std::memcpy(&dir_fade, d + 28, 4);
      std::memcpy(&fog_clip, d + 32, 4);
      std::memcpy(&fog_pow, d + 36, 4);
      std::printf("\n  fogNearDist=%.0f fogFarDist=%.0f dirRot xy=%d z=%d dirFade=%.2f "
                  "fogClip=%.0f fogPow=%.2f",
                  fog_near, fog_far, rot_xy, rot_z, dir_fade, fog_clip, fog_pow);
    }
    if (n >= 80) {
      float fog_max;
      std::memcpy(&fog_max, d + 76, 4);
      std::printf("\n  DALC dir X+=%s specular=%s  fogFar=%s fogMax=%.2f", col(d + 40).c_str(),
                  col(d + 64).c_str(), col(d + 72).c_str(), fog_max);
    }
    if (n >= 92) {
      float fade_begin, fade_end;
      rec::u32 inherits;
      std::memcpy(&fade_begin, d + 80, 4);
      std::memcpy(&fade_end, d + 84, 4);
      std::memcpy(&inherits, d + 88, 4);
      std::printf("\n  lightFade %.0f..%.0f  inherits=0x%03x [", fade_begin, fade_end, inherits);
      const char* names[] = {"ambient", "directional", "fogColor", "fogNear",
                             "fogFar",  "dirRot",       "dirFade",  "clip",
                             "fogPow",  "fogMax",       "lightFade"};
      for (int b = 0; b < 11; ++b)
        if (inherits & (1u << b)) std::printf("%s ", names[b]);
      std::printf("]");
    }
    std::printf("\n");
  });
  std::printf("interior cells: %d, with XCLL/LTMP shown: %d (XCLL present: %d)\n", interiors, shown,
              with_xcll);
  return 0;
}

// Decodes each LGTM lighting-template record's DATA block (the XCLL layout minus
// the inherit flags) plus its DALC directional-ambient subrecord, to cross-check
// the XCLL offsets and see the template values interiors inherit.
// esminfo <data-dir> lgtm [limit].
int DumpLightingTemplates(const std::string& data_dir, int limit) {
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) return 1;

  constexpr rec::u32 kDalc = rec::FourCc('D', 'A', 'L', 'C');
  auto col = [](const rec::u8* d) {
    return std::string(std::to_string(d[0]) + "," + std::to_string(d[1]) + "," +
                       std::to_string(d[2]));
  };
  int shown = 0, total = 0;
  records.EachOfType(rec::FourCc('L', 'G', 'T', 'M'),
                     [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    ++total;
    if (limit > 0 && shown >= limit) return;
    Record r;
    if (!records.Parse(id, &r)) return;
    const Subrecord* data = r.Find(kData);
    if (!data || data->data.size() < 40) return;
    ++shown;
    const rec::u8* d = data->data.data();
    float fog_near, fog_far, fog_pow;
    std::memcpy(&fog_near, d + 12, 4);
    std::memcpy(&fog_far, d + 16, 4);
    std::memcpy(&fog_pow, d + 36, 4);
    std::printf("LGTM %04x:%06x %-28s data=%zu ambient=%s directional=%s fogNear=%s "
                "near=%.0f far=%.0f pow=%.2f",
                id.plugin, id.local_id, r.GetString(kEdid).c_str(),
                static_cast<size_t>(data->data.size()), col(d).c_str(), col(d + 4).c_str(),
                col(d + 8).c_str(), fog_near, fog_far, fog_pow);
    if (data->data.size() >= 80) std::printf(" fogFar=%s", col(d + 72).c_str());
    if (const Subrecord* dalc = r.Find(kDalc); dalc && dalc->data.size() >= 24)
      std::printf(" DALC X+=%s Z-=%s", col(dalc->data.data()).c_str(),
                  col(dalc->data.data() + 20).c_str());
    std::printf("\n");
  });
  std::printf("total LGTM: %d\n", total);
  return 0;
}

// Generic: dumps each record of a four-character type with its editor id and
// subrecord (type, size) list, for reverse-engineering an unfamiliar record.
// esminfo <data-dir> dump <TYPE> [limit].
int DumpType(const std::string& data_dir, const std::string& type, int limit) {
  if (type.size() != 4) {
    std::printf("type must be 4 chars\n");
    return 1;
  }
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) return 1;

  rec::u32 fourcc = rec::FourCc(type[0], type[1], type[2], type[3]);
  int shown = 0, total = 0;
  records.EachOfType(fourcc, [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    ++total;
    if (limit > 0 && shown >= limit) return;
    Record r;
    if (!records.Parse(id, &r)) return;
    ++shown;
    std::printf("%s %04x:%06x %s\n", type.c_str(), id.plugin, id.local_id,
                r.GetString(kEdid).c_str());
    for (const Subrecord& sub : r.subrecords) {
      char t[5] = {};
      std::memcpy(t, &sub.type, 4);
      // Show the first 4 bytes as a form ref / int for short subrecords.
      rec::u32 head = 0;
      if (sub.data.size() >= 4) std::memcpy(&head, sub.data.data(), 4);
      std::printf("  %s  %4zu  [%08x]\n", t, static_cast<size_t>(sub.data.size()), head);
    }
  });
  std::printf("total %s: %d\n", type.c_str(), total);
  return 0;
}

}  // namespace

// Dumps header info and record counts of a plugin. Handy for checking the
// parser against real game files. With a data directory and a cell
// coordinate it lists that exterior cell's refs instead.
int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: esminfo <plugin.esm> [game: skyrimse|fo4|fo76]\n");
    std::printf("       esminfo <data-dir> cell <x,y>\n");
    std::printf("       esminfo <data-dir> gras\n");
    return 1;
  }

  if (argc >= 3 && std::string(argv[2]) == "gras") return DumpGrass(argv[1]);

  if (argc >= 3 && std::string(argv[2]) == "wthr")
    return DumpWeather(argv[1], argc >= 4 ? std::stoi(argv[3]) : 0);

  if (argc >= 3 && std::string(argv[2]) == "watr")
    return DumpWater(argv[1], argc >= 4 ? std::stoi(argv[3]) : 0);

  if (argc >= 3 && std::string(argv[2]) == "cellwater")
    return DumpCellWater(argv[1], argc >= 4 ? std::stoi(argv[3]) : 0);

  if (argc >= 3 && std::string(argv[2]) == "txstrefs")
    return DumpTxstRefs(argv[1], argc >= 4 ? std::stoi(argv[3]) : 0);

  if (argc >= 3 && std::string(argv[2]) == "xcll") {
    int limit = 0;
    std::string name;
    if (argc >= 4) {
      const std::string arg = argv[3];
      if (!arg.empty() && std::all_of(arg.begin(), arg.end(), ::isdigit))
        limit = std::stoi(arg);
      else
        name = arg;
    }
    return DumpInteriorLighting(argv[1], limit, name);
  }

  if (argc >= 3 && std::string(argv[2]) == "lgtm")
    return DumpLightingTemplates(argv[1], argc >= 4 ? std::stoi(argv[3]) : 0);

  if (argc >= 4 && std::string(argv[2]) == "dump")
    return DumpType(argv[1], argv[3], argc >= 5 ? std::stoi(argv[4]) : 0);

  if (argc >= 4 && std::string(argv[2]) == "land") {
    std::string coords = argv[3];
    size_t comma = coords.find(',');
    if (comma == std::string::npos) return 1;
    return DumpLand(argv[1], std::stoi(coords.substr(0, comma)),
                    std::stoi(coords.substr(comma + 1)));
  }

  if (argc >= 4 && std::string(argv[2]) == "cell") {
    std::string coords = argv[3];
    size_t comma = coords.find(',');
    if (comma == std::string::npos) return 1;
    return DumpCellRefs(argv[1], std::stoi(coords.substr(0, comma)),
                        std::stoi(coords.substr(comma + 1)));
  }

  Game game = Game::kSkyrimSe;
  if (argc > 2) {
    std::string id = argv[2];
    if (id == "fo4") game = Game::kFallout4;
    if (id == "fo76") game = Game::kFallout76;
  }

  auto plugin = PluginFile::Open(argv[1], GameProfile::For(game));
  if (!plugin) return 1;

  std::printf("%s\n", plugin->file_name().c_str());
  std::printf("  version: %.2f\n", plugin->version());
  std::printf("  master:  %s\n", plugin->is_master() ? "yes" : "no");
  std::printf("  light:   %s\n", plugin->is_light() ? "yes" : "no");
  std::printf("  records: %u (header)\n", plugin->record_count());
  for (const auto& master : plugin->masters()) {
    std::printf("  requires %s\n", master.c_str());
  }

  std::map<std::string, int> counts;
  plugin->VisitRecords([&](Record& record) {
    char type[5] = {};
    std::memcpy(type, &record.header.type, 4);
    ++counts[type];
  });
  for (const auto& [type, count] : counts) {
    std::printf("  %s x%d\n", type.c_str(), count);
  }
  return 0;
}
