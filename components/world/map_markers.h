#ifndef RECREATION_WORLD_MAP_MARKERS_H_
#define RECREATION_WORLD_MAP_MARKERS_H_

// The named places on the world map, and which of them the player has found.
//
// A marker is a REFR the plugin places with an ExtraMapMarker on it: the record
// carries the name, the icon and the position, and the save carries nothing but
// two flags per marker (visible, can travel to). So the catalogue is built from
// records once and the flags are then set from a savegame or by walking up to
// one; both write the same field and neither invents an entry.
//
// Discovery only ever grows, exactly like MapDiscovery's cell bits, so loading
// a save over a walked-in world keeps whatever each of them knew.

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "components/bethesda/form_id.h"
#include "core/types.h"

namespace rx::world {

struct MapMarker {
  bethesda::GlobalFormId ref;         // the REFR that places it
  bethesda::GlobalFormId worldspace;  // the worldspace it stands in
  // The worldspace whose map it draws on. A city worldspace shares its parent's
  // coordinates and appears on the parent's map, so Whiterun's marker belongs to
  // WhiterunWorld but is drawn on Tamriel.
  bethesda::GlobalFormId map_worldspace;
  base::String name;
  f32 position[3] = {};  // game units, the REFR's own DATA
  u8 icon = 0;           // TNAM marker type, i.e. which symbol the map draws
  bool visible = false;
  bool can_travel = false;
  // The place has been finished off. Not a property of the marker at all: it
  // belongs to the LCTN the marker stands for, and whoever knows that link sets
  // it here so the map can draw it without a location graph of its own.
  bool cleared = false;
};

class MapMarkers {
 public:
  // From the records. A second Add for the same reference replaces it, so a
  // rebuild is idempotent.
  void Add(const MapMarker& marker);

  // What a savegame recorded. Flags only ever turn on: a save that has not seen
  // a marker the player already walked past must not hide it again. False when
  // no marker with that reference is known.
  bool SetFlags(bethesda::GlobalFormId ref, bool visible, bool can_travel);
  // Found by standing near it: visible and travelable at once, which is what the
  // game writes when the player discovers a location. Returns true only the
  // first time, so the caller can announce it.
  bool Discover(bethesda::GlobalFormId ref);
  // Marks the place behind a marker as finished off. Like the flags above this
  // only ever turns on. False when no marker with that reference is known.
  bool SetCleared(bethesda::GlobalFormId ref);

  const base::Vector<MapMarker>& all() const { return markers_; }
  const MapMarker* Find(bethesda::GlobalFormId ref) const;
  u32 VisibleCount() const;
  u32 TravelableCount() const;
  u32 ClearedCount() const;

  // The nearest marker to a point in game units, searched in one worldspace and
  // within `radius` units. Null when nothing is in range. Undiscovered only,
  // because this is what turns walking into discovery.
  const MapMarker* NearestUndiscovered(bethesda::GlobalFormId worldspace,
                                       const f32 position[3],
                                       f32 radius) const;

  void Clear();

 private:
  MapMarker* FindMutable(bethesda::GlobalFormId ref);

  base::Vector<MapMarker> markers_;
  base::UnorderedMap<u64, u32> by_ref_;  // packed reference id -> index
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_MAP_MARKERS_H_
