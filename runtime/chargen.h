#ifndef RECREATION_RUNTIME_CHARGEN_H_
#define RECREATION_RUNTIME_CHARGEN_H_

#include <memory>
#include <string>
#include <vector>

#include <base/containers/vector.h>

#include "bethesda/facegen.h"
#include "bethesda/form_id.h"
#include "core/math.h"
#include "core/types.h"
#include "ecs/entity.h"
#include "face.h"
#include "game_ui.h"

namespace rx {

struct EngineContext;
struct InputState;

// A Bethesda-style character-creation screen that runs inside the engine: boots a
// single FaceGen head into a lit void, frames a camera on it, and drives the head
// live from a ugui overlay (race / sex / RPRM preset, the 18 NAM9 face sliders, an
// advanced page of NAMA type cyclers + chargen morphs, and a look page of skin
// tone + hair style/colour). CharGen owns all the interaction: it hit-tests its
// own fixed-geometry panels (game_ui.h kCg* constants) against the raw cursor, so
// the overlay is a pure view. A slider drag re-morphs the head every frame
// (RebuildAndUpload, a few ms); skin tone re-bakes on release (the bake versions
// GPU ids). Boots via RX_CHARGEN (see content_load.cc); mirrors MapEditor.
class CharGen {
 public:
  explicit CharGen(EngineContext& ctx);
  ~CharGen();

  bool active() const { return active_; }

  // Build the race/hair catalogs, assemble the base preview head, frame the
  // camera and open the overlay. Loads a saved preset (RX_CHARGEN_OUT, default
  // chargen_preset.json) when one exists. A no-op with no resolvable races.
  void Enter();

  // Per-frame tick while active: applies RX_CHARGEN_SCRIPT once, hit-tests the
  // panels (slider drag / cyclers / race / sex / preset / page / actions), orbits
  // the camera on a viewport drag (wheel zooms head<->bust), and pushes the
  // CharGenView into the overlay.
  void Update(const InputState& input, f32 dt);

 private:
  // One playable race the preview can blend to (all share the human head mesh).
  struct Race {
    bethesda::GlobalFormId form;
    std::string edid;     // RACE EDID, the race-blend morph name
    std::string display;  // shortened label
  };
  // One hairstyle HDPT valid for the current race/sex.
  struct Hair {
    std::string model;  // MODL nif path
    std::string edid;
  };

  // A single control on the active page: a drag slider (NAM9 / morph / skin
  // channel) or a wraparound cycler (NAMA type / hairstyle / hair colour).
  struct Control {
    enum class Kind { kNam9, kMorph, kSkin, kNamaType, kHairStyle, kHairColor };
    Kind kind;
    std::string label;
    std::string name;      // chargen morph name, for kMorph
    int slot = 0;          // NAM9 index / NAMA slot / skin channel
    f32 mn = -1, mx = 1;   // slider value range
  };

  // --- setup ---
  void BuildRaces();
  void BuildHairCatalog();  // hair styles for the current sex + AHCM colours
  // Reassemble the preview head for the current race/sex/preset. read_preset
  // seeds the sliders from the assembled preset's authored face; otherwise the
  // current slider mirror is kept (used when loading a saved preset).
  void Reassemble(bool read_preset);
  void SpawnHeadEntities();
  void RebuildHairGroom();
  // The head bone's rest world position (engine space), read once from the real
  // skeleton: hair HDPT NIFs are authored head-bone-local, so the preview groom
  // is translated by this onto the standalone head.
  Vec3 HeadBoneOffset();
  void PushEditsToFace();       // mirror -> FaceState (all sliders + types + skin)
  void ScanChargenMorphs();     // available morph names + NAMA type index lists
  void SetupSceneAndCamera();   // portrait light + framed orbit camera (once)

  // --- interaction ---
  std::vector<Control> BuildControls() const;  // the active page's controls
  void ApplyControl(const Control& c, f32 value01);  // slider drag (0..1)
  void CycleControl(const Control& c, int dir);      // cycler step (+1 / -1)
  void Randomize();
  void Reset();
  int PresetCount() const { return preset_count_; }
  void SelectRace(int i);
  void SelectSex(int sex);
  void StepPreset(int dir);
  void ApplyScript(const std::string& script);

  // --- persistence ---
  void Save();
  bool Load();  // returns true if a saved preset was applied

  // --- view + camera ---
  void PushView();
  void UpdateCameraPose();

  EngineContext& ctx_;
  bool active_ = false;

  std::unique_ptr<FaceBuilder> builder_;
  FaceState face_;
  base::Vector<ecs::Entity> head_entities_;
  u32 groom_ = 0;

  std::vector<Race> races_;
  int race_ = 0;
  int sex_ = 0;      // 0 male, 1 female
  int preset_ = 0;   // index into the current sex head's RPRM presets
  int preset_count_ = 0;
  int page_ = 0;     // 0 Face, 1 Advanced, 2 Look
  int row_first_ = 0;

  // Slider mirror, pushed into FaceState. Kept here so the UI reflects a freshly
  // assembled preset and a saved preset overrides it.
  f32 nam9_[bethesda::kNam9Count] = {};
  i32 nama_[4] = {-1, -1, -1, -1};
  f32 skin_[3] = {0.6f, 0.5f, 0.45f};
  std::vector<std::pair<std::string, f32>> morphs_;  // advanced chargen morphs

  // Chargen-tri capabilities of the assembled head.
  std::vector<std::string> morph_names_;   // every chargen morph on the head
  std::vector<std::string> curated_morphs_;  // the advanced page's morph subset
  std::vector<int> nose_types_, eyes_types_, mouth_types_;  // NAMA index lists (0 = base)

  std::vector<Hair> hair_styles_;
  int hair_style_ = 0;
  std::vector<std::pair<std::string, Vec3>> hair_colors_;  // AHCM CLFM colours
  int hair_color_ = -1;  // -1 = the assembled default

  // Framed portrait orbit around the head (limits + defaults in chargen.cc).
  Vec3 head_center_{0.0f, 1.63f, 0.0f};
  f32 orbit_yaw_ = 0.35f;
  f32 orbit_pitch_ = 0.0f;
  f32 orbit_dist_ = 0.56f;

  // Interaction state.
  bool prev_lmb_ = false;
  bool dragging_ = false;
  int drag_control_ = -1;   // index into the active page's controls
  bool drag_skin_ = false;  // the drag is a skin channel (rebake on release)
  bool orbiting_ = false;
  f32 prev_mx_ = 0, prev_my_ = 0;

  std::string status_;
  f32 status_age_ = 999.0f;
  std::string save_path_;

  bool scene_ready_ = false;
  bool script_done_ = false;
  f32 elapsed_ = 0.0f;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_CHARGEN_H_
