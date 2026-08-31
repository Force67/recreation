#ifndef RECREATION_WEATHER_WEATHER_LOADER_H_
#define RECREATION_WEATHER_WEATHER_LOADER_H_

#include <base/containers/pair.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "components/bethesda/load_order.h"
#include "components/weather/weather.h"

namespace rx::weather {

// Parses every WTHR (weather) record into a def map keyed by packed form id, and
// returns the count. Defensive: the physical params come from the DATA
// classification flags (pleasant / cloudy / rainy / snow), with an editor-id
// fallback. Bethesda's baked sky/cloud textures in the record are ignored - only
// the parameters and the authored intent are kept.
int LoadWeathers(const bethesda::RecordStore& records, base::UnorderedMap<u64, WeatherDef>* out);

// Which colour treatment a weather ended up with, reported so a caller counting
// them does not have to repeat the rule.
enum class WeatherGrade : u8 { kNone, kRadstorm, kColorGrade };

// One WTHR, for a caller that already knows which. False when the id names no
// WTHR record. LoadWeathers is this in a loop; it exists on its own because
// resuming a savegame needs the one weather the save was left under and has no
// use for the other hundred.
bool LoadWeather(const bethesda::RecordStore& records,
                 bethesda::GlobalFormId id,
                 WeatherDef* out,
                 WeatherGrade* grade = nullptr);

// Builds a weighted climate (the input a WeatherSystem expects) for the named
// worldspace: its authored CLMT weather list (WRLD CNAM -> CLMT WLST), else the
// CLMT with the most resolvable weathers, else a synthetic spread over the
// loaded weather kinds. Empty when no weathers loaded.
//
// `min_worldspace_weathers` is how many weathers the worldspace's own CLMT must
// resolve before it is trusted over the synthetic spread. Skyrim keeps it high
// (its thin Tamriel CLMT yields to synthetic + REGN region overrides); Starfield
// passes 1 because each planet authors a single characteristic surface weather.
base::Vector<base::Pair<WeatherDef, u32>> BuildClimate(
    const bethesda::RecordStore& records,
    const base::UnorderedMap<u64, WeatherDef>& weathers,
    const char* worldspace_edid,
    int min_worldspace_weathers = 4);

// Parses the REGN weather regions of `worldspace` (their area polygons + weather
// lists) into `out`. Returns the count. The active region's climate overrides
// the worldspace default where the player stands.
int LoadRegions(const bethesda::RecordStore& records,
                const base::UnorderedMap<u64, WeatherDef>& weathers,
                bethesda::GlobalFormId worldspace,
                RegionWeather* out);

}  // namespace rx::weather

#endif  // RECREATION_WEATHER_WEATHER_LOADER_H_
