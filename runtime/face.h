#ifndef RECREATION_RUNTIME_FACE_H_
#define RECREATION_RUNTIME_FACE_H_

#include <string>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>

#include "asset/mesh.h"
#include "bethesda/facegen.h"
#include "bethesda/form_id.h"
#include "bethesda/head_morph.h"
#include "bethesda/tri.h"
#include "core/types.h"

namespace rec {

struct EngineContext;

namespace bethesda {
struct TriMorphSet;
}

// One built head-part mesh ready to place: the uploaded renderable id and the
// part type (so a caller can order/attach eyes vs face vs hair).
struct BuiltFacePart {
  asset::AssetId mesh;
  bethesda::HeadPartType type = bethesda::HeadPartType::kMisc;
  bool skin = false;  // face/eyes/brows/beard skin part (vs hair/misc)
};

class FaceState;

// Resolves, caches and builds Skyrim FaceGen heads at runtime. One instance per
// engine owns the tri + base-mesh caches shared across every assembled face.
// AssembleNpc turns an NPC_ into an editable FaceState; FaceState::RebuildAnd
// Upload does the per-edit morph + Loop subdivision + upload.
class FaceBuilder {
 public:
  explicit FaceBuilder(EngineContext& ctx);

  // Assembles a whole NPC face into `out`: merges the race's default head parts
  // with the NPC's PNAM overrides (by part type), loads each part's base NIF and
  // race/chargen tris (cached, textures+materials uploaded once), and seeds the
  // weights from the NPC's NAM9/NAMA/race. Returns false when the NPC or its
  // race head data can't be resolved. Does not upload geometry; the caller runs
  // out->RebuildAndUpload().
  bool AssembleNpc(bethesda::GlobalFormId npc, FaceState* out);

 private:
  friend class FaceState;
  const bethesda::TriMorphSet* Tri(const std::string& vfs_path);
  const asset::Mesh* BasePartMesh(const std::string& model_path);

  // UniquePointer values so a FaceState may cache raw pointers into the pointees:
  // the map rehashes on later inserts, but that only moves the pointers, never
  // the heap objects behind them. A cached entry with 0 verts / empty lods marks
  // an absent (failed/missing) tri or mesh.
  EngineContext& ctx_;
  base::UnorderedMap<u64, base::UniquePointer<bethesda::TriMorphSet>> tri_cache_;
  base::UnorderedMap<u64, base::UniquePointer<asset::Mesh>> mesh_cache_;
};

// A mutable head, edited live by the chargen UI: hold the resolved parts + base
// data, mutate weights, RebuildAndUpload. Fast enough for slider dragging
// (morph + subdivide + normals on an ~900-vertex head is well under a frame).
class FaceState {
 public:
  // --- Live weights (all take effect on the next RebuildAndUpload) ---
  void SetNam9(u32 index, f32 value);        // index < bethesda::kNam9Count
  void SetNama(u32 slot, i32 index);         // slot 0..3: nose, brows, eyes, mouth
  // A direct chargen morph override, layered on top of the NAM9-derived morphs
  // (e.g. "NoseLong" 3.0 for an exaggerated nose, or any chargen tri morph the
  // UI exposes). weight 0 removes the override.
  void SetMorph(const std::string& chargen_morph, f32 weight);
  void SetRaceBlend(const std::string& race_morph);  // e.g. "OrcRace"; "" disables
  void SetSubdivLevels(u32 levels);

  // Rebuilds every part (copy base -> race blend -> chargen morphs -> Loop
  // subdivide -> recompute normals/tangents) and re-uploads under stable ids so
  // an entity Renderable keeps pointing at the same mesh. Returns the wall time
  // taken, in milliseconds, for perf reporting.
  f32 RebuildAndUpload();

  const base::Vector<BuiltFacePart>& parts() const { return built_; }
  bool female() const { return female_; }
  const f32* skin_tone() const { return skin_tone_; }
  const std::string& race_morph() const { return race_morph_; }
  u32 subdiv_levels() const { return subdiv_levels_; }

  // Chargen morph names available on this face's chargen tri, for the UI to
  // populate its "advanced" slider list beyond the 18 NAM9 sliders.
  base::Vector<std::string> ChargenMorphNames() const;

 private:
  friend class FaceBuilder;
  struct Part {
    bethesda::HeadPartType type = bethesda::HeadPartType::kMisc;
    const asset::Mesh* base = nullptr;                     // FaceBuilder-owned
    const bethesda::TriMorphSet* race_tri = nullptr;
    const bethesda::TriMorphSet* chargen_tri = nullptr;
    bool subdivide = true;
    asset::AssetId out_id;  // stable per face+part; re-uploaded on every edit
    std::string label;
  };

  FaceBuilder* builder_ = nullptr;
  base::Vector<Part> parts_;
  std::string race_morph_;
  bool female_ = false;
  f32 nam9_[bethesda::kNam9Count] = {};
  i32 nama_[4] = {-1, -1, -1, -1};
  f32 skin_tone_[3] = {0.6f, 0.5f, 0.45f};
  base::Vector<bethesda::MorphWeight> extra_;  // direct SetMorph overrides
  u32 subdiv_levels_ = 1;
  base::Vector<BuiltFacePart> built_;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_FACE_H_
