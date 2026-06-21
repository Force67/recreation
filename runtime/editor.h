#ifndef RECREATION_RUNTIME_EDITOR_H_
#define RECREATION_RUNTIME_EDITOR_H_

#include <base/containers/vector.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bethesda/form_id.h"
#include "core/math.h"
#include "core/types.h"
#include "ecs/entity.h"
#include "game_ui.h"
#include "thumbnailer.h"
#include "world/components.h"

namespace rec::bethesda {
class RecordStore;
class StringTable;
}  // namespace rec::bethesda
namespace rec::world {
class CellStreamer;
}  // namespace rec::world
namespace rec::render {
struct PointLight;
}  // namespace rec::render
namespace rec::asset {
struct Mesh;
}  // namespace rec::asset

namespace rec {

struct EngineContext;
struct InputState;

// One game whose assets the editor can browse and place. The primary (rendered)
// game is domain 0; each `--add-game` content domain adds another, so a Fallout
// 4 prop can be dropped into the Skyrim world (the streamer converts/uploads it
// with that domain's own vfs and per-domain mesh salt, so it renders correctly
// beside the primary game's content). `tag` is a stable slug stored in the
// layout file so a saved placement reloads against the right game.
struct EditorPlaceDomain {
  std::string name;
  std::string tag;
  bethesda::RecordStore* records = nullptr;
  bethesda::StringTable* strings = nullptr;
  world::CellStreamer* streamer = nullptr;
};

// Asset browser category tabs. Index 0 ("All") aggregates the rest; each catalog
// entry stores its specific bucket (1..N). Shared by the catalog builder and the
// view so the tab order never drifts.
inline constexpr const char* kEditorCategories[] = {
    "All",   "Statics", "Furniture", "Doors",      "Containers",
    "Flora", "Lights",  "Clutter",   "Activators",
};
inline constexpr int kEditorCategoryCount =
    sizeof(kEditorCategories) / sizeof(kEditorCategories[0]);

// A live, in-game map editor: a Creation Kit that runs inside the streamed
// world. Toggle it on (F4) and the camera becomes a free-fly builder; an asset
// browser lists every placeable base form pulled from the loaded game files,
// and a click drops the chosen asset onto the terrain. Selected objects can be
// moved, rotated, scaled and deleted, with an undo stack. All the logic lives
// here; the UI is mirrored through GameUi (EditorView) and drives back through
// a single event sink, so the renderer and ECS need no editor-specific code.
//
// The editor operates on the primary content domain (ctx.streamer / ctx.records
// / ctx.assets). It is a windowed-client feature; a headless build never
// constructs one.
class MapEditor {
 public:
  explicit MapEditor(EngineContext& ctx);

  bool active() const { return active_; }

  // True while a search box (asset browser or scene tree) is focused, so the
  // engine routes the keyboard to text entry instead of flying the camera
  // (mirrors the debug overlay's wants_keyboard()).
  bool wants_keyboard() const { return active_ && (search_focused_ || scene_search_focused_); }

  // Receives a click/scroll on an editor widget (registered with GameUi as the
  // editor event sink). Public so the GameUi callback can reach it.
  void HandleUiEvent(const EditorUiEvent& e);

  // The games whose assets the editor can place (domain 0 = primary). The engine
  // wires this once data has loaded; if never called the editor falls back to
  // the primary content domain alone.
  void SetPlaceDomains(std::vector<EditorPlaceDomain> domains);

  // Enter or leave editor mode. Entering drops walk mode, frees the cursor and
  // arms the overlay; leaving clears the selection and the armed brush.
  void Toggle();

  // Per-frame editor tick (only meaningful while active). `allow_input` is false
  // when the pause menu owns input. Reads the frame's input, applies edits, and
  // pushes the rebuilt EditorView into the HUD.
  void Update(const InputState& input, f32 dt, bool allow_input);

  // Appends a dynamic point light for every placed LIGH form (with its record's
  // colour and radius), so a dropped torch or lamp actually lights the scene.
  // Called by the engine each frame whether or not the editor is open, so placed
  // lights stay lit during play. A no-op when nothing light-shaped was placed.
  void CollectLights(base::Vector<render::PointLight>& out) const;

 private:
  // One placeable base form discovered in the load order.
  struct CatalogEntry {
    bethesda::GlobalFormId base;
    std::string name;       // displayed FULL name, falling back to the editor id
    std::string editor_id;  // EDID
    u32 type = 0;           // record fourcc (STAT, TREE, ...)
    int category = 0;       // index into the category tabs
    int domain = 0;         // which game it came from (index into domains_)
  };

  // An object the editor placed (so it can be counted, re-selected and, later,
  // serialized). Streamed refs the editor edits are tracked only transiently.
  // category/type/editor_id mirror the catalog entry so the scene tree can group
  // it and the inspector can name its model (defaulted when unknown).
  struct PlacedObject {
    ecs::Entity entity;
    bethesda::GlobalFormId base;
    std::string name;
    int domain = 0;
    int category = 0;
    u32 type = 0;
    std::string editor_id;
  };

  // One flattened scene-tree row's target, parallel to the pushed view rows, so
  // a click resolves back to the root / a category group / a placed object.
  struct TreeNode {
    int kind = 0;  // 0 root, 1 group, 2 leaf
    int category = 0;
    ecs::Entity entity = ecs::kInvalidEntity;
  };

  // One part of a reusable group ("prefab"): a placeable form plus its transform
  // relative to the group anchor, so the whole group can be stamped as a unit.
  struct PrefabMember {
    bethesda::GlobalFormId base;
    int domain = 0;
    f32 rel[3] = {0, 0, 0};  // offset from the anchor at capture
    f32 rot[4] = {0, 0, 0, 1};
    f32 scale = 1.0f;  // native multiplier
  };

  // The inverse of one edit, for the undo stack.
  enum class UndoKind { kPlace, kDelete, kTransform };
  struct UndoOp {
    UndoKind kind;
    ecs::Entity entity;           // kPlace/kTransform target
    bethesda::GlobalFormId base;  // kDelete/kPlace base form
    world::Transform transform;   // kTransform: prior transform; kDelete: where it was
    std::string name;
    int domain = 0;  // kDelete: the game to re-place from
  };

  // --- catalog (editor_catalog.cc) ---
  void BuildCatalog();
  void RefreshFilter();  // recompute filtered_ from search_ + category_
  void EnsureDomains();  // default domains_ to the primary domain if unset

  // --- asset thumbnails (thumbnailer.cc renderer + on-disk PNG cache) ---
  // Renders a few pending previews per frame for the visible cards; cached to
  // disk so later boots load instantly. ThumbTexFor returns the ready texture
  // id (or 0). MeshForCatalog converts/loads an entry's model on demand.
  void GenerateThumbnails();
  u64 ThumbTexFor(u64 base_packed) const;  // ready texture id for a base form, or 0
  const asset::Mesh* MeshForCatalog(const CatalogEntry& e);

  // The streamer that converts/uploads/places a given domain's assets.
  world::CellStreamer* StreamerFor(int domain) const;

  // Emissive parameters of a LIGH base form, parsed once and cached.
  struct LightParams {
    f32 color[3] = {1.0f, 0.9f, 0.7f};
    f32 radius = 5.0f;  // metres
    f32 intensity = 6.0f;
  };
  const LightParams* LightFor(bethesda::GlobalFormId base, int domain) const;

  // --- persistence (editor_io.cc) ---
  // The layout file is a tiny line-based record of placed objects (base form +
  // engine-space transform), so a building survives a restart. SaveLayout writes
  // placed_; LoadLayout re-places each line through the streamer. Both report a
  // status message and return the count written/read.
  int SaveLayout();
  int LoadLayout();

  // --- input / ops (editor.cc) ---
  void UpdateSearchInput(const InputState& input);  // text entry while focused
  void ApplyKeyboard(const InputState& input);
  void ArmBrush(int catalog_index);
  void PlaceBrush(const InputState& input);
  // Spawns the armed asset at an engine-space point facing `yaw`, tracking it for
  // save/undo. Shared by single placement and paint-scatter.
  ecs::Entity PlaceArmedAt(const Vec3& pos, f32 yaw);
  f32 ScatterYaw();  // a varied, deterministic yaw per scattered object
  // A live preview of the armed asset following the aim point, so placing reads
  // as dragging it onto the world. ClearGhost destroys it.
  void UpdateGhost(const InputState& input);
  void ClearGhost();
  // Drops a curated row of assets on the ground ahead of the camera and saves
  // the layout. Driven by REC_EDITOR_DEMO so a capture (or a save/load round
  // trip) needs no interactive clicks.
  void PlaceDemoBuild();
  void SelectUnderCursor(const InputState& input, bool additive);
  void DeleteSelection();
  void DuplicateSelection();
  void RotateSelection(f32 radians);
  void ScaleSelection(f32 factor);
  void BeginMove();  // snapshot the selection's transforms and start a grab
  // Prefabs: capture the current multi-selection as a reusable group, then stamp
  // copies of the whole group at the aim point (Ctrl+G groups, click stamps).
  void SaveSelectionAsPrefab();
  void StampPrefab(const Vec3& at);
  void Undo();
  void Redo();
  // Performs one undo/redo step's action and returns the op that reverses it, so
  // undo and redo share one symmetric path (the inverse just moves to the other
  // stack). Mutates the world / placed_ / selected_.
  UndoOp ApplyAndInvert(const UndoOp& op);
  void PushEdit(const UndoOp& op);           // record an edit; clears the redo stack
  void RecordTransform(ecs::Entity entity);  // push a kTransform undo snapshot
  void SetStatus(std::string message);
  void PushView();

  // --- picking / placement geometry ---
  bool PointerOverUi(const InputState& input) const;
  Vec3 CursorRayDir(const InputState& input) const;
  bool AimPoint(const InputState& input, Vec3* out) const;            // ray vs ground
  Vec3 Snap(const Vec3& p) const;                                     // grid-snap x/z when snap_
  ecs::Entity PickEntity(const InputState& input, f32* out_t) const;  // ray vs spheres
  bool ProjectToScreen(const Vec3& world, f32* sx, f32* sy) const;    // world -> window px
  void BoxSelect(f32 x0, f32 y0, f32 x1, f32 y1, bool additive);      // marquee select
  ecs::Entity Primary() const;            // the active selection (inspector/reticle/move)
  world::Transform* SelectedTransform();  // the primary's transform
  void PruneDeadSelection();              // drop selected entities that were destroyed
  int FindPlaced(ecs::Entity e) const;    // index into placed_, or -1

  EngineContext& ctx_;
  bool active_ = false;

  // The games whose assets can be placed (domain 0 = primary game).
  std::vector<EditorPlaceDomain> domains_;

  // Catalog + browser filter state.
  std::vector<CatalogEntry> catalog_;
  std::vector<int> filtered_;  // indices into catalog_ matching search_/category_
  bool catalog_built_ = false;
  int category_ = 0;
  std::string search_;
  int page_first_ = 0;           // scroll offset into filtered_
  int brush_ = -1;               // catalog_ index of the armed asset, or -1
  f32 brush_yaw_ = 0;            // yaw the next placement faces (R orients it)
  bool search_focused_ = false;  // asset search box has keyboard focus
  bool snap_ = false;            // snap placements / moves to a ground grid
  f32 snap_grid_ = 1.0f;         // grid size in metres when snap_ is on
  int grid_index_ = 1;           // index into the cycle of grid sizes

  // Left dock: Scene tree / Assets tabs, a tree search box, per-group expand
  // state and a scroll window into the flattened tree.
  int left_tab_ = 0;  // 0 = Scene, 1 = Assets
  std::string scene_search_;
  bool scene_search_focused_ = false;
  bool root_expanded_ = true;
  bool group_expanded_[kEditorCategoryCount] = {};  // set true in the ctor
  int tree_scroll_ = 0;
  std::vector<TreeNode> tree_targets_;  // visible window, parallel to view rows

  // Asset-card thumbnails: rendered on demand (a few per frame), cached to disk.
  std::unique_ptr<Thumbnailer> thumber_;
  bool thumber_tried_ = false;
  std::unordered_map<u64, u64> thumb_tex_;  // base form id -> ugui TextureId
  std::unordered_set<u64> thumb_failed_;    // base form ids that won't render
  std::string thumb_dir_;

  // Live placement preview ("ghost"): a transient entity, excluded from picking
  // and never saved, that tracks the aim point while a brush is armed.
  ecs::Entity ghost_entity_ = ecs::kInvalidEntity;
  int ghost_brush_ = -1;  // catalog index the ghost currently shows

  // Selection (a set, for multi-select) + active tool. The "primary" is the last
  // one added; the inspector, reticle and move pivot follow it. Shift-click adds
  // or removes; a plain click replaces.
  std::vector<ecs::Entity> selected_;
  int tool_ = 0;         // 0 select, 1 move, 2 rotate, 3 scale
  bool moving_ = false;  // G-grab gesture: the selection follows the aim point
  std::vector<world::Transform> move_origins_;  // per-selected transform at grab start
  Vec3 move_pivot_{};                           // primary's position at grab start
  bool prev_lmb_ = false;                       // left-button edge detection (clicks, not holds)

  // Marquee box-select: a click-drag in empty space; on release everything whose
  // screen projection falls in the rect is selected.
  bool marquee_dragging_ = false;
  f32 marquee_x0_ = 0, marquee_y0_ = 0, marquee_x1_ = 0, marquee_y1_ = 0;

  // Current prefab (a reusable group) and whether a click stamps it.
  std::vector<PrefabMember> prefab_;
  bool prefab_armed_ = false;

  // base form id -> emissive params (nullopt = not a light), built lazily.
  mutable std::unordered_map<u64, std::optional<LightParams>> light_cache_;

  // Paint-scatter: holding the place button and dragging drops a copy every
  // `scatter_spacing_` metres, each at a varied yaw, for fast forests / clutter.
  Vec3 last_scatter_pos_{};
  f32 scatter_spacing_ = 1.5f;
  u32 scatter_count_ = 0;

  std::vector<PlacedObject> placed_;
  std::vector<UndoOp> undo_;
  std::vector<UndoOp> redo_;  // cleared by any fresh edit (PushEdit)

  std::string status_;
  f32 status_age_ = 0;
  u32 next_synth_id_ = 1;  // local id for placed objects' synthetic form links

  std::string layout_path_;     // where SaveLayout/LoadLayout read and write
  bool layout_loaded_ = false;  // auto-load the saved layout once, on first entry
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_EDITOR_H_
