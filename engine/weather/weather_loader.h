#ifndef RECREATION_WEATHER_WEATHER_LOADER_H_
#define RECREATION_WEATHER_WEATHER_LOADER_H_

#include <unordered_map>
#include <utility>
#include <vector>

#include "bethesda/load_order.h"
#include "weather/weather.h"

namespace rx::weather {

// Parses every WTHR (weather) record into a def map keyed by packed form id, and
// returns the count. Defensive: the physical params come from the DATA
// classification flags (pleasant / cloudy / rainy / snow), with an editor-id
// fallback. Bethesda's baked sky/cloud textures in the record are ignored - only
// the parameters and the authored intent are kept.
int LoadWeathers(const bethesda::RecordStore& records, std::unordered_map<u64, WeatherDef>* out);

// Builds a weighted climate (the input a WeatherSystem expects) for the named
// worldspace: its authored CLMT weather list (WRLD CNAM -> CLMT WLST), else the
// CLMT with the most resolvable weathers, else a synthetic spread over the
// loaded weather kinds. Empty when no weathers loaded.
//
// `min_worldspace_weathers` is how many weathers the worldspace's own CLMT must
// resolve before it is trusted over the synthetic spread. Skyrim keeps it high
// (its thin Tamriel CLMT yields to synthetic + REGN region overrides); Starfield
// passes 1 because each planet authors a single characteristic surface weather.
std::vector<std::pair<WeatherDef, u32>> BuildClimate(
    const bethesda::RecordStore& records, const std::unordered_map<u64, WeatherDef>& weathers,
    const char* worldspace_edid, int min_worldspace_weathers = 4);

// Parses the REGN weather regions of `worldspace` (their area polygons + weather
// lists) into `out`. Returns the count. The active region's climate overrides
// the worldspace default where the player stands.
int LoadRegions(const bethesda::RecordStore& records,
                const std::unordered_map<u64, WeatherDef>& weathers,
                bethesda::GlobalFormId worldspace, RegionWeather* out);

}  // namespace rx::weather

#endif  // RECREATION_WEATHER_WEATHER_LOADER_H_
