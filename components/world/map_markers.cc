#include "components/world/map_markers.h"

namespace rx::world {

void MapMarkers::Add(const MapMarker& marker) {
  if (u32* index = by_ref_.find(marker.ref.packed())) {
    markers_[*index] = marker;
    return;
  }
  by_ref_[marker.ref.packed()] = static_cast<u32>(markers_.size());
  markers_.push_back(marker);
}

MapMarker* MapMarkers::FindMutable(bethesda::GlobalFormId ref) {
  u32* index = by_ref_.find(ref.packed());
  return index ? &markers_[*index] : nullptr;
}

const MapMarker* MapMarkers::Find(bethesda::GlobalFormId ref) const {
  const u32* index = by_ref_.find(ref.packed());
  return index ? &markers_[*index] : nullptr;
}

bool MapMarkers::SetFlags(bethesda::GlobalFormId ref, bool visible, bool can_travel) {
  MapMarker* marker = FindMutable(ref);
  if (!marker)
    return false;
  marker->visible = marker->visible || visible;
  marker->can_travel = marker->can_travel || can_travel;
  return true;
}

bool MapMarkers::Discover(bethesda::GlobalFormId ref) {
  MapMarker* marker = FindMutable(ref);
  if (!marker || marker->visible)
    return false;
  marker->visible = true;
  marker->can_travel = true;
  return true;
}

bool MapMarkers::SetCleared(bethesda::GlobalFormId ref) {
  MapMarker* marker = FindMutable(ref);
  if (!marker)
    return false;
  marker->cleared = true;
  return true;
}

u32 MapMarkers::VisibleCount() const {
  u32 count = 0;
  for (const MapMarker& marker : markers_)
    count += marker.visible ? 1 : 0;
  return count;
}

u32 MapMarkers::TravelableCount() const {
  u32 count = 0;
  for (const MapMarker& marker : markers_)
    count += marker.can_travel ? 1 : 0;
  return count;
}

u32 MapMarkers::ClearedCount() const {
  u32 count = 0;
  for (const MapMarker& marker : markers_)
    count += marker.cleared ? 1 : 0;
  return count;
}

const MapMarker* MapMarkers::NearestUndiscovered(bethesda::GlobalFormId worldspace,
                                                 const f32 position[3],
                                                 f32 radius) const {
  const MapMarker* best = nullptr;
  f32 best_distance = radius * radius;
  for (const MapMarker& marker : markers_) {
    if (marker.visible || marker.worldspace.packed() != worldspace.packed())
      continue;
    const f32 dx = marker.position[0] - position[0];
    const f32 dy = marker.position[1] - position[1];
    const f32 dz = marker.position[2] - position[2];
    const f32 distance = dx * dx + dy * dy + dz * dz;
    if (distance > best_distance)
      continue;
    best_distance = distance;
    best = &marker;
  }
  return best;
}

void MapMarkers::Clear() {
  markers_.clear();
  by_ref_.clear();
}

}  // namespace rx::world
