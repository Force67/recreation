#ifndef RECREATION_WORLD_TERRAIN_EDITS_H_
#define RECREATION_WORLD_TERRAIN_EDITS_H_

#include <compare>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace rx::world {

// Bethesda LAND cells have 32 quads and 33 samples per side. Samples are stored
// once on this global lattice, so a point on a cell edge is automatically shared:
// global_x = cell_x * 32 + local_col, likewise for y/row.
struct TerrainCellKey {
  i32 x = 0;
  i32 y = 0;

  bool operator==(const TerrainCellKey&) const = default;
  auto operator<=>(const TerrainCellKey&) const = default;
};

struct TerrainSampleKey {
  i32 x = 0;
  i32 y = 0;

  bool operator==(const TerrainSampleKey&) const = default;
  auto operator<=>(const TerrainSampleKey&) const = default;
};

enum class TerrainBrushMode : u8 { kRaise, kLower, kSmooth, kFlatten };

struct TerrainBrush {
  // Center/radius are measured in canonical LAND sample intervals. Strength and
  // flatten_target are Bethesda game-height units. Smooth/flatten interpret
  // strength as a 0..1 blend amount; raise/lower add/subtract it directly.
  f32 center_x = 0;
  f32 center_y = 0;
  f32 radius = 1;
  f32 strength = 1;
  f32 falloff = 1;
  f32 flatten_target = 0;
  TerrainBrushMode mode = TerrainBrushMode::kRaise;
};

struct TerrainSampleChange {
  TerrainSampleKey sample;
  f32 old_delta = 0;
  f32 new_delta = 0;
};

struct TerrainEditChange {
  std::vector<TerrainSampleChange> samples;
  // Every cell that displays one of samples, including cardinal/diagonal cells
  // sharing an edge or corner. Sorted and unique.
  std::vector<TerrainCellKey> cells;

  bool empty() const { return samples.empty(); }
};

// Sparse final height deltas for one Bethesda worldspace. This unit owns the
// canonical lattice, brush semantics, reversible changes and the .recterrain
// representation; it never owns or mutates original plugin/archive data.
class TerrainEdits {
 public:
  using BaseHeight =
      std::function<std::optional<f32>(i32 global_x, i32 global_y)>;
  using FingerprintLookup =
      std::function<std::optional<u64>(TerrainCellKey cell)>;

  void BindWorld(std::string identity);
  const std::string& world_identity() const { return world_identity_; }

  f32 SampleDelta(i32 global_x, i32 global_y) const;
  bool AffectsCell(TerrainCellKey cell) const;
  bool ComposeCell(TerrainCellKey cell, std::span<const f32> base_heights,
                   std::span<f32> composed_heights) const;

  TerrainEditChange ApplyBrush(const TerrainBrush& brush,
                               const BaseHeight& base_height);
  bool ApplyChange(const TerrainEditChange& change);
  bool RevertChange(const TerrainEditChange& change);
  // Removes every sample and returns the already-applied reversible change.
  TerrainEditChange Clear();

  void SetCellFingerprint(TerrainCellKey cell, u64 fingerprint);
  std::optional<u64> CellFingerprint(TerrainCellKey cell) const;

  size_t sample_count() const { return samples_.size(); }
  size_t fingerprint_count() const { return fingerprints_.size(); }
  bool dirty() const { return dirty_; }
  std::vector<TerrainCellKey> dirty_cells() const;
  std::vector<TerrainCellKey> touched_cells() const;
  void MarkSaved();

 private:
  bool SetChangeState(const TerrainEditChange& change, bool use_new);

  std::string world_identity_;
  std::map<TerrainSampleKey, f32> samples_;
  std::map<TerrainCellKey, u64> fingerprints_;
  std::map<TerrainCellKey, bool> dirty_cells_;
  bool dirty_ = false;

  friend bool SaveTerrainEdits(const TerrainEdits&, const std::string&,
                               std::string*);
  friend bool LoadTerrainEdits(const std::string&, std::string_view,
                               const FingerprintLookup&, TerrainEdits*,
                               std::string*);
};

// Appends an already-applied dab to a stroke. Repeated samples retain the
// stroke's first old delta and the dab's final delta.
bool MergeTerrainEditChanges(TerrainEditChange* stroke,
                             const TerrainEditChange& dab);

// Versioned little-endian Recreation terrain diff (.recterrain). Save writes a
// sibling temporary file then renames it over the destination.
bool SaveTerrainEdits(const TerrainEdits& edits, const std::string& file_path,
                      std::string* error = nullptr);
bool LoadTerrainEdits(const std::string& file_path,
                      std::string_view expected_world_identity,
                      const TerrainEdits::FingerprintLookup& fingerprints,
                      TerrainEdits* edits, std::string* error = nullptr);

}  // namespace rx::world

#endif  // RECREATION_WORLD_TERRAIN_EDITS_H_
