#ifndef RECREATION_RUNTIME_FACE_H_
#define RECREATION_RUNTIME_FACE_H_

#include <string>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>

#include "asset/mesh.h"
#include "asset/texture.h"
#include "bethesda/facegen.h"
#include "bethesda/form_id.h"
#include "bethesda/head_morph.h"
#include "bethesda/tri.h"
#include "core/types.h"

namespace rx {

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

  // The diffuse/normal/specular texture paths a converted head-part NIF bound,
  // captured so the facetint bake can reuse the racial skin as its base and the
  // head material can keep the authored normal/spec.
  struct PartTextures {
    std::string diffuse;
    std::string normal;
    std::string specular;
  };
  const PartTextures* Textures(const std::string& model_path);

  // A texture decoded to a tight rgb8 grid (sRGB bytes) for CPU compositing.
  // Empty (w==0) marks a load/decode failure so misses are cached too.
  struct Decoded {
    u32 w = 0;
    u32 h = 0;
    base::Vector<u8> rgb;  // w*h*3, sRGB
  };
  const Decoded* DecodedTexture(const std::string& path);

  // UniquePointer values so a FaceState may cache raw pointers into the pointees:
  // the map rehashes on later inserts, but that only moves the pointers, never
  // the heap objects behind them. A cached entry with 0 verts / empty lods marks
  // an absent (failed/missing) tri or mesh.
  EngineContext& ctx_;
  base::UnorderedMap<u64, base::UniquePointer<bethesda::TriMorphSet>> tri_cache_;
  base::UnorderedMap<u64, base::UniquePointer<asset::Mesh>> mesh_cache_;
  base::UnorderedMap<u64, PartTextures> part_tex_;
  base::UnorderedMap<u64, base::UniquePointer<Decoded>> decoded_cache_;
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
  // Live skin tone (QNAM), for the chargen slider: marks the facetint dirty so
  // the next BakeFaceTint re-composites. Values are sRGB-ish 0..1 multipliers.
  void SetSkinTone(f32 r, f32 g, f32 b);

  // Composites the racial face diffuse * skin tone + every tint layer into a
  // per-NPC face texture, uploads it and a skin material, and points the face
  // parts at it. Cheap to repeat (base + masks stay decoded in the builder), so
  // a chargen slider can call it live. No-op with nothing to bake. Returns ms.
  f32 BakeFaceTint();

  // Rebuilds every part (copy base -> race blend -> chargen morphs -> Loop
  // subdivide -> recompute normals/tangents) and re-uploads under stable ids so
  // an entity Renderable keeps pointing at the same mesh. Returns the wall time
  // taken, in milliseconds, for perf reporting.
  f32 RebuildAndUpload();

  const base::Vector<BuiltFacePart>& parts() const { return built_; }
  bool female() const { return female_; }
  // The live NAM9 slider values (kNam9Count) and NAMA type indices (nose/brows/
  // eyes/mouth), so the chargen UI can read back a freshly assembled preset's
  // face instead of showing neutral sliders over a morphed head.
  const f32* nam9() const { return nam9_; }
  const i32* nama() const { return nama_; }
  const f32* skin_tone() const { return skin_tone_; }
  // The NPC's hair HDPT nif path (empty when bald), for the caller to build a
  // strand groom instead of the flat card the part would otherwise be.
  const std::string& hair_model() const { return hair_model_; }
  // HCLF/CLFM hair colour, 0..1 rgb, for tinting the groom + brows/beard.
  const f32* hair_color() const { return hair_color_; }
  const std::string& race_morph() const { return race_morph_; }
  u32 subdiv_levels() const { return subdiv_levels_; }
  // The face part's object-space (Bethesda game units) bounding sphere, set by
  // RebuildAndUpload. A hair groom, authored head-local at the origin, is dropped
  // onto the scalp by anchoring it here (the face part carries the body-height
  // offset the standalone hair NIF lacks).
  const f32* head_center() const { return head_center_; }
  f32 head_radius() const { return head_radius_; }

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
    asset::AssetId material_override;  // baked face/tint material (0 = keep NIF)
    std::string model;   // HDPT nif path, for the part's own texture lookup
    std::string label;
  };

  // One resolved tint layer: an sRGB colour painted through a race mask texture
  // at 0..1 strength, over the skin-tone base.
  struct TintLayer {
    std::string mask;   // race TINT mask path (grayscale coverage)
    f32 color[3] = {1, 1, 1};  // NPC TINC, sRGB 0..1
    f32 alpha = 0;             // NPC TINV / 100
    u16 type = 0;              // race TINP mask type
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

  // Facetint bake inputs + outputs. The base diffuse/normal come from the face
  // part's NIF (racial skin); the tint layers from the NPC + race records.
  std::string face_diffuse_;
  std::string face_normal_;
  base::Vector<TintLayer> tint_layers_;
  asset::AssetId face_texture_;   // baked per-NPC diffuse (0 until baked)
  asset::AssetId face_material_;  // baked per-NPC skin material
  std::string npc_tag_;
  std::string hair_model_;
  f32 hair_color_[3] = {0.32f, 0.24f, 0.18f};
  f32 head_center_[3] = {0, 0, 0};  // face part bounds, game units (hair anchor)
  f32 head_radius_ = 0;
  u32 tint_version_ = 0;
  bool tint_dirty_ = true;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_FACE_H_
