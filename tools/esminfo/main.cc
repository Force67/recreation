#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
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
