// map_markerstest: the discovered-location store. The markers used here are the
// real Skyrim.esm ones, with the ids, names, icons and positions those records
// hold, so a wrong key or a swapped field shows up as a wrong place rather than
// as a passing tautology.

#include <cstdio>

#include "components/world/map_markers.h"

namespace {

using rx::f32;
using rx::bethesda::GlobalFormId;
using rx::world::MapMarker;
using rx::world::MapMarkers;

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

constexpr GlobalFormId kTamriel{0, 0x0000003C};
constexpr GlobalFormId kWhiterunWorld{0, 0x0001A26F};

// REFR 0x00017780 Helgen, in Tamriel, icon 2 (town).
MapMarker Helgen() {
  MapMarker m;
  m.ref = GlobalFormId{0, 0x00017780};
  m.worldspace = kTamriel;
  m.map_worldspace = kTamriel;
  m.name = "Helgen";
  m.position[0] = 18350.4004f;
  m.position[1] = -79480.9688f;
  m.position[2] = 9204.4111f;
  m.icon = 2;
  return m;
}

// REFR 0x000162CE Whiterun, placed in WhiterunWorld but drawn on Tamriel's map.
MapMarker Whiterun() {
  MapMarker m;
  m.ref = GlobalFormId{0, 0x000162CE};
  m.worldspace = kWhiterunWorld;
  m.map_worldspace = kTamriel;
  m.name = "Whiterun";
  m.position[0] = 19855.2266f;
  m.position[1] = -7422.5103f;
  m.position[2] = -3582.6194f;
  m.icon = 40;
  return m;
}

// REFR 0x000162A4 Riverwood, the village up the road from Helgen.
MapMarker Riverwood() {
  MapMarker m;
  m.ref = GlobalFormId{0, 0x000162A4};
  m.worldspace = kTamriel;
  m.map_worldspace = kTamriel;
  m.name = "Riverwood";
  m.position[0] = 21941.0f;
  m.position[1] = -44792.0f;
  m.position[2] = -140.0f;
  m.icon = 2;
  return m;
}

void TestCatalogue() {
  MapMarkers markers;
  markers.Add(Helgen());
  markers.Add(Whiterun());
  Check("two markers", markers.all().size() == 2);
  Check("a marker is found by its reference",
        markers.Find(GlobalFormId{0, 0x00017780}) != nullptr &&
            markers.Find(GlobalFormId{0, 0x000162CE})->name == "Whiterun");
  Check("an unknown reference finds nothing",
        markers.Find(GlobalFormId{0, 0x00000014}) == nullptr);
  Check("Whiterun stands in its own worldspace but draws on Tamriel's map",
        markers.Find(GlobalFormId{0, 0x000162CE})->worldspace.packed() ==
                kWhiterunWorld.packed() &&
            markers.Find(GlobalFormId{0, 0x000162CE})->map_worldspace.packed() ==
                kTamriel.packed());

  // A rebuild must not double the catalogue.
  markers.Add(Helgen());
  Check("re-adding a marker replaces it", markers.all().size() == 2);
  Check("nothing is discovered yet",
        markers.VisibleCount() == 0 && markers.TravelableCount() == 0);
}

void TestFlags() {
  MapMarkers markers;
  markers.Add(Helgen());
  markers.Add(Whiterun());

  // What a save carries: the two flags off the ExtraMapMarker.
  Check("flags apply to a known marker",
        markers.SetFlags(GlobalFormId{0, 0x00017780}, true, true));
  Check("flags on an unknown reference are refused",
        !markers.SetFlags(GlobalFormId{0, 0x00099999}, true, true));
  Check("Helgen is now a travel destination",
        markers.VisibleCount() == 1 && markers.TravelableCount() == 1);

  // A civil war camp: on the map, but not travelable.
  markers.SetFlags(GlobalFormId{0, 0x000162CE}, true, false);
  Check("visible without travel", markers.VisibleCount() == 2 && markers.TravelableCount() == 1);
  // Discovery only grows, so a later save that says less cannot take it away.
  markers.SetFlags(GlobalFormId{0, 0x00017780}, false, false);
  Check("clearing flags does not un-discover",
        markers.Find(GlobalFormId{0, 0x00017780})->can_travel);
}

void TestDiscovery() {
  MapMarkers markers;
  markers.Add(Helgen());
  markers.Add(Riverwood());

  const f32 outside_helgen[3] = {18000.0f, -79000.0f, 9200.0f};
  const MapMarker* near = markers.NearestUndiscovered(kTamriel, outside_helgen, 3000.0f);
  Check("standing outside Helgen finds Helgen", near && near->name == "Helgen");
  // Riverwood is 34000 units up the road, well past the radius.
  Check("Riverwood is not in range",
        markers.NearestUndiscovered(kTamriel, outside_helgen, 3000.0f)->ref.local_id == 0x00017780);
  Check("nothing is in range of the far side of the map",
        markers.NearestUndiscovered(kTamriel, outside_helgen, 100.0f) == nullptr);
  Check("another worldspace's coordinates find nothing",
        markers.NearestUndiscovered(kWhiterunWorld, outside_helgen, 3000.0f) == nullptr);

  Check("discovering it reports the first time",
        markers.Discover(GlobalFormId{0, 0x00017780}));
  Check("and only the first time", !markers.Discover(GlobalFormId{0, 0x00017780}));
  Check("a discovered marker can be travelled to",
        markers.Find(GlobalFormId{0, 0x00017780})->visible &&
            markers.Find(GlobalFormId{0, 0x00017780})->can_travel);
  Check("a discovered marker is no longer searched for",
        markers.NearestUndiscovered(kTamriel, outside_helgen, 3000.0f) == nullptr);
}

}  // namespace

int main() {
  std::puts("map_markerstest");
  TestCatalogue();
  TestFlags();
  TestDiscovery();
  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}
