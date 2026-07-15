#include "weather/weather_loader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "bethesda/record.h"
#include "core/log.h"

namespace rx::weather {
namespace {

constexpr u32 kWthr = FourCc('W', 'T', 'H', 'R');
constexpr u32 kNam0 = FourCc('N', 'A', 'M', '0');
constexpr u32 kClmt = FourCc('C', 'L', 'M', 'T');
constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kWlst = FourCc('W', 'L', 'S', 'T');
constexpr u32 kCnam = FourCc('C', 'N', 'A', 'M');
constexpr u32 kRegn = FourCc('R', 'E', 'G', 'N');
constexpr u32 kWnam = FourCc('W', 'N', 'A', 'M');
constexpr u32 kRpld = FourCc('R', 'P', 'L', 'D');
constexpr u32 kRdat = FourCc('R', 'D', 'A', 'T');
constexpr u32 kRdwt = FourCc('R', 'D', 'W', 'T');
constexpr u32 kSnam = FourCc('S', 'N', 'A', 'M');
constexpr u32 kFnam = FourCc('F', 'N', 'A', 'M');

// WTHR DATA classification flags byte (offset 11 in the Skyrim/FO4 layout).
WeatherDef::Kind Classify(const bethesda::Record& rec, const std::string& edid) {
  if (const bethesda::Subrecord* data = rec.Find(kData); data && data->data.size() >= 12) {
    u8 cls = data->data[11];
    if (cls & 0x08) return WeatherDef::Kind::kSnow;
    if (cls & 0x04) return WeatherDef::Kind::kRainy;
    if (cls & 0x02) return WeatherDef::Kind::kCloudy;
    if (cls & 0x01) return WeatherDef::Kind::kPleasant;
  }
  // Fallback: vanilla editor ids are descriptive (SkyrimRainStorm, ...).
  std::string l;
  for (char c : edid) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto has = [&](const char* s) { return l.find(s) != std::string::npos; };
  if (has("snow") || has("blizzard")) return WeatherDef::Kind::kSnow;
  if (has("rain") || has("storm") || has("thunder")) return WeatherDef::Kind::kRainy;
  if (has("cloud") || has("overcast") || has("fog")) return WeatherDef::Kind::kCloudy;
  return WeatherDef::Kind::kPleasant;
}

// FO4's radiation storms: a sickly-green irradiated haze with frequent
// lightning, not rain. Recognised by editor id (RadStorm / *Radiation*) and
// tuned onto our physical knobs. Returns true when applied.
bool ApplyRadstorm(WeatherDef* def) {
  std::string l;
  for (char c : def->editor_id) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (l.find("radstorm") == std::string::npos && l.find("radiation") == std::string::npos)
    return false;
  def->cloud_coverage = 0.88f;
  def->cloud_density = 1.3f;
  def->aerosol = 0.9f;            // thick green murk -> heavy aerial perspective
  def->light_scale = 0.5f;        // dim, oppressive
  def->light_tint = {0.66f, 0.85f, 0.45f};  // irradiated green cast
  def->precipitation = 0.0f;      // dust/radiation, not rain (no wet surfaces)
  def->snow = false;
  def->thunder = true;            // frequent green-tinted strikes
  return true;
}

// Refines the light tint from the weather's authored colours. Skyrim's NAM0 is
// a fixed 17-component colour block, each component holding 4 times-of-day
// (sunrise/day/dusk/night) as RGBA. Component 4 is the directional Sunlight
// colour; we take its day variant and normalise it to a pure colour cast, so
// each weather lights the scene with its authored mood while our physical sun
// keeps its intensity. Guarded to the 272-byte Skyrim-era layout (FO4's is 608
// bytes / 38 components with a different order, and resolves a real climate
// anyway). Skipped if the colour is unset. Returns true when applied.
bool ApplyColorGrade(const bethesda::Record& rec, WeatherDef* def) {
  const bethesda::Subrecord* nam0 = rec.Find(kNam0);
  if (!nam0 || nam0->data.size() != 272) return false;
  const u8* c = nam0->data.data() + (4 * 4 + 1) * 4;  // component 4 (Sunlight), day
  f32 r = c[0], g = c[1], b = c[2];
  f32 m = std::max(r, std::max(g, b));
  if (m < 1.0f) return false;  // black/unset -> keep the classification tint
  // Pure cast (max channel = 1), with a floor so a saturated authored colour
  // never darkens a channel to near-black.
  def->light_tint = {std::max(0.45f, r / m), std::max(0.45f, g / m), std::max(0.45f, b / m)};
  return true;
}

u64 Mix64(u64 x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

f32 Clamp01(f32 v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Decodes the WTHR gameplay knobs beyond the classification: DATA's wind
// direction/thunder cadence/lightning colour, the SNAM sound list and FNAM's fog
// distances. Layout verified against Skyrim.esm (see tools/esminfo DumpWeather).
void DecodeAuthoredFields(const bethesda::RecordStore& records, const bethesda::Record& rec,
                          u16 plugin, WeatherDef* def) {
  constexpr f32 kTau = 6.28318530718f;

  // SNAM entries: (sound form, kind) pairs - 0 default, 1 precip, 2 wind,
  // 3 thunder. The thunder pool keeps every entry so strikes can vary.
  for (const bethesda::Subrecord& sub : rec.subrecords) {
    if (sub.type != kSnam || sub.data.size() < 8) continue;
    u32 raw = 0, sound_kind = 0;
    std::memcpy(&raw, sub.data.data(), 4);
    std::memcpy(&sound_kind, sub.data.data() + 4, 4);
    const bethesda::GlobalFormId snd = records.ResolveFrom(bethesda::RawFormId{raw}, plugin);
    if (snd.plugin == 0xffff || snd.local_id == 0) continue;
    if (sound_kind == 1)
      def->sound_precip = snd.packed();
    else if (sound_kind == 2)
      def->sound_wind = snd.packed();
    else if (sound_kind == 3)
      def->sound_thunder.push_back(snd.packed());
  }

  bool lightning_authored = false;
  if (const bethesda::Subrecord* data = rec.Find(kData); data && data->data.size() >= 15) {
    const u8* d = data->data.data();
    def->wind = static_cast<f32>(d[0]) / 255.0f * 30.0f;  // byte -> m/s

    // Lightning colour (bytes 12..14), normalised to a pure cast so the flash
    // brightness stays the renderer's. All-zero means unauthored: thundery
    // weathers then keep the bluish-white default.
    const f32 lr = d[12], lg = d[13], lb = d[14];
    const f32 lm = std::max(lr, std::max(lg, lb));
    if (lm >= 1.0f) {
      def->lightning_color = {lr / lm, lg / lm, lb / lm};
      lightning_authored = true;
    }

    // Thundery when the record says so (a thunder sound or an authored lightning
    // colour); the editor-id heuristic in Classify/DeriveFromKind stays as the
    // fallback for records carrying neither (FO4 radstorms keep their flag).
    std::string l;
    for (char c : def->editor_id) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool edid_storm =
        l.find("storm") != std::string::npos || l.find("thunder") != std::string::npos;
    def->thunder = !def->sound_thunder.empty() || lightning_authored || (def->thunder && edid_storm);

    // Thunder frequency byte: LOWER is MORE frequent. Map onto a strike period
    // of roughly 4..20 s; the director jitters each strike deterministically.
    if (def->thunder)
      def->thunder_period = 4.0f + 16.0f * (static_cast<f32>(d[10]) / 255.0f);
    else
      def->thunder_period = -1.0f;

    // Wind direction byte -> yaw, plus a per-record offset hashed inside the
    // authored direction range so two weathers sharing a direction still drift
    // apart (deterministic: the form id seeds the hash).
    if (data->data.size() >= 18) {
      f32 yaw = static_cast<f32>(d[16]) / 255.0f * kTau;
      const f32 range = static_cast<f32>(d[17]) / 255.0f * kTau;
      const u64 h = Mix64(def->form);
      const f32 frac = static_cast<f32>((h >> 16) & 0xffff) / 65535.0f - 0.5f;
      yaw = std::fmod(yaw + frac * range, kTau);
      def->wind_yaw = yaw < 0 ? yaw + kTau : yaw;
    }

    // Gustiness: the classification's baseline plus the authored wind; storms
    // and blizzards squall.
    def->gustiness = Clamp01(def->gustiness + def->wind / 30.0f * 0.4f);
    if (def->thunder && def->precipitation > 0.0f)
      def->gustiness = std::max(def->gustiness, 0.75f);
    if (def->snow && def->wind > 15.0f) def->gustiness = std::max(def->gustiness, 0.85f);
  }

  // FNAM fog distances refine the haze: a near fog-far plane means dense air.
  // Conservative (only ever raises aerosol, capped) so existing looks survive.
  if (const bethesda::Subrecord* fnam = rec.Find(kFnam); fnam && fnam->data.size() >= 16) {
    f32 f[4];
    std::memcpy(f, fnam->data.data(), 16);
    // The day fog-far plane is the larger of floats 1/2 across the two known
    // orderings (near/far vs day/night interleave); good enough for a bound.
    const f32 far_day = std::max(f[1], f[2]);
    if (far_day > 1.0f && far_day < 10000.0f)
      def->aerosol = std::max(def->aerosol, Clamp01((10000.0f - far_day) / 10000.0f) * 0.6f);
  }
}

bool ReadFormRef(const bethesda::RecordStore& records, const bethesda::Record& rec, u32 sub,
                 u16 plugin, bethesda::GlobalFormId* out) {
  const bethesda::Subrecord* s = rec.Find(sub);
  if (!s || s->data.size() < 4) return false;
  u32 raw;
  std::memcpy(&raw, s->data.data(), 4);
  *out = records.ResolveFrom(bethesda::RawFormId{raw}, plugin);
  return out->plugin != 0xffff;
}

// One WLST stride: each entry is (weather form id, chance u32, ...). The trailing
// fields differ across games (Skyrim has no per-entry global, Fallout/Starfield
// do), so the entry size varies; weather id and chance stay at offsets 0 and 4.
std::vector<std::pair<WeatherDef, u32>> ParseWlst(
    const bethesda::RecordStore& records, const u8* p, size_t sz, u16 plugin, size_t stride,
    const std::unordered_map<u64, WeatherDef>& weathers) {
  std::vector<std::pair<WeatherDef, u32>> out;
  for (size_t off = 0; off + stride <= sz; off += stride) {
    u32 raw, chance;
    std::memcpy(&raw, p + off, 4);
    std::memcpy(&chance, p + off + 4, 4);
    bethesda::GlobalFormId wid = records.ResolveFrom(bethesda::RawFormId{raw}, plugin);
    auto it = weathers.find(wid.packed());
    if (it != weathers.end() && chance > 0 && chance <= 100) out.push_back({it->second, chance});
  }
  return out;
}

// A weighted weather list (CLMT WLST or REGN RDWT) parsed at whichever entry
// stride resolves the most weathers (the trailing fields differ across games).
std::vector<std::pair<WeatherDef, u32>> BestWlst(
    const bethesda::RecordStore& records, const u8* p, size_t sz, u16 plugin,
    const std::unordered_map<u64, WeatherDef>& weathers) {
  std::vector<std::pair<WeatherDef, u32>> best;
  for (size_t stride : {8u, 12u, 16u}) {
    auto list = ParseWlst(records, p, sz, plugin, stride, weathers);
    if (list.size() > best.size()) best = std::move(list);
  }
  return best;
}

// A climate's WLST weather list.
std::vector<std::pair<WeatherDef, u32>> FromClimate(
    const bethesda::RecordStore& records, bethesda::GlobalFormId climate,
    const std::unordered_map<u64, WeatherDef>& weathers) {
  const bethesda::RecordStore::StoredRecord* stored = records.Find(climate);
  bethesda::Record rec;
  if (!stored || !records.Parse(climate, &rec)) return {};
  const bethesda::Subrecord* wlst = rec.Find(kWlst);
  if (!wlst) return {};
  return BestWlst(records, wlst->data.data(), wlst->data.size(), stored->winning_plugin, weathers);
}

std::vector<std::pair<WeatherDef, u32>> Synthetic(
    const std::unordered_map<u64, WeatherDef>& weathers) {
  const WeatherDef* rep[4] = {nullptr, nullptr, nullptr, nullptr};
  for (auto& [id, def] : weathers) {
    int k = static_cast<int>(def.kind);
    if (k >= 0 && k < 4 && !rep[k]) rep[k] = &def;
  }
  const u32 weight[4] = {50, 30, 12, 8};  // pleasant, cloudy, rainy, snow
  std::vector<std::pair<WeatherDef, u32>> out;
  for (int k = 0; k < 4; ++k)
    if (rep[k]) out.push_back({*rep[k], weight[k]});
  return out;
}

}  // namespace

int LoadWeathers(const bethesda::RecordStore& records, std::unordered_map<u64, WeatherDef>* out) {
  int n = 0, rad = 0, graded = 0;
  bool storm_logged = false;
  records.EachOfType(
      kWthr, [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord& stored) {
        bethesda::Record rec;
        if (!records.Parse(id, &rec)) return;
        WeatherDef def;
        def.form = id.packed();
        def.editor_id = rec.GetString(kEdid);
        def.kind = Classify(rec, def.editor_id);
        def.DeriveFromKind();
        if (ApplyRadstorm(&def))
          ++rad;  // radstorm's deliberate green cast wins over the authored grade
        else if (ApplyColorGrade(rec, &def))
          ++graded;
        DecodeAuthoredFields(records, rec, stored.winning_plugin, &def);
        // One decoded storm logged in full: an end-to-end check of the DATA/SNAM
        // parse against real data without a debugger.
        if (!storm_logged && !def.sound_thunder.empty()) {
          storm_logged = true;
          RX_INFO(
              "weather: storm '{}' decoded -- wind {:.1f} m/s yaw {:.2f} rad gust {:.2f} "
              "thunder every ~{:.1f}s lightning {:.2f}/{:.2f}/{:.2f} sounds precip {:x} wind {:x} "
              "thunder x{}",
              def.editor_id, def.wind, def.wind_yaw, def.gustiness, def.thunder_period,
              def.lightning_color.x, def.lightning_color.y, def.lightning_color.z,
              def.sound_precip, def.sound_wind, def.sound_thunder.size());
        }
        (*out)[def.form] = std::move(def);
        ++n;
      });
  if (rad > 0) RX_INFO("weather: {} radiation storms", rad);
  if (graded > 0) RX_INFO("weather: {} weathers colour-graded from authored sunlight", graded);
  return n;
}

std::vector<std::pair<WeatherDef, u32>> BuildClimate(
    const bethesda::RecordStore& records, const std::unordered_map<u64, WeatherDef>& weathers,
    const char* worldspace_edid, int min_worldspace_weathers) {
  if (weathers.empty()) return {};

  // 1) The worldspace's authored climate (WRLD CNAM -> CLMT).
  bethesda::GlobalFormId wrld = records.FindWorldspace(worldspace_edid);
  if (wrld.plugin != 0xffff) {
    bethesda::Record wrec;
    const bethesda::RecordStore::StoredRecord* ws = records.Find(wrld);
    if (ws && records.Parse(wrld, &wrec)) {
      bethesda::GlobalFormId climate;
      if (ReadFormRef(records, wrec, kCnam, ws->winning_plugin, &climate)) {
        auto list = FromClimate(records, climate, weathers);
        // Use the authored climate when it resolves enough weathers. Skyrim sets
        // the bar high (its thin Tamriel CLMT yields to the richer synthetic
        // spread + REGN); Starfield sets it to 1 (a planet authors one weather).
        if (static_cast<int>(list.size()) >= min_worldspace_weathers && !list.empty()) {
          RX_INFO("weather: climate from worldspace {} ({} weathers)", worldspace_edid,
                   list.size());
          return list;
        }
      }
    }
  }

  // 2) The CLMT with the most resolvable weathers.
  std::vector<std::pair<WeatherDef, u32>> best;
  records.EachOfType(kClmt,
                     [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
                       auto list = FromClimate(records, id, weathers);
                       if (list.size() > best.size()) best = std::move(list);
                     });
  if (best.size() >= 4) {
    RX_INFO("weather: climate from largest CLMT ({} weathers)", best.size());
    return best;
  }

  // 3) Synthetic spread over the loaded weather kinds.
  auto syn = Synthetic(weathers);
  RX_INFO("weather: synthetic climate ({} kinds)", syn.size());
  return syn;
}

int LoadRegions(const bethesda::RecordStore& records,
                const std::unordered_map<u64, WeatherDef>& weathers,
                bethesda::GlobalFormId worldspace, RegionWeather* out) {
  if (weathers.empty() || worldspace.plugin == 0xffff) return 0;
  int n = 0;
  records.EachOfType(
      kRegn, [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord& stored) {
        bethesda::Record rec;
        if (!records.Parse(id, &rec)) return;
        // Only regions in this worldspace.
        bethesda::GlobalFormId ws;
        if (!ReadFormRef(records, rec, kWnam, stored.winning_plugin, &ws) || !(ws == worldspace))
          return;

        Region region;
        region.form = id.packed();
        // Subrecords are ordered: the area polygon (RPLD) precedes the data
        // sections, each an RDAT header followed by its payload. The weather
        // section is RDAT type 3, whose RDWT lists are the region's weathers.
        bool in_weather = false;
        for (const bethesda::Subrecord& sub : rec.subrecords) {
          if (sub.type == kRpld) {
            const u8* p = sub.data.data();
            for (size_t k = 0; k + 8 <= sub.data.size(); k += 8) {
              f32 x, y;
              std::memcpy(&x, p + k, 4);
              std::memcpy(&y, p + k + 4, 4);
              region.polygon.push_back({x, y});
            }
          } else if (sub.type == kRdat) {
            u32 type = 0;
            if (sub.data.size() >= 4) std::memcpy(&type, sub.data.data(), 4);
            in_weather = (type == 3);
            if (in_weather && sub.data.size() >= 6) region.priority = sub.data.data()[5];
          } else if (in_weather && sub.type == kRdwt) {
            auto list =
                BestWlst(records, sub.data.data(), sub.data.size(), stored.winning_plugin, weathers);
            for (auto& e : list) region.climate.push_back(std::move(e));
          }
        }
        if (!region.climate.empty() && region.polygon.size() >= 3) {
          out->Add(std::move(region));
          ++n;
        }
      });
  return n;
}

}  // namespace rx::weather
