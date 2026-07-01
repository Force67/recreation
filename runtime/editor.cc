#include "editor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <utility>

#include <base/option.h>

#include "asset/asset_database.h"
#include "asset/asset_id.h"
#include "asset/mesh.h"
#include "bethesda/record.h"
#include "core/input.h"
#include "core/log.h"
#include "ecs/world.h"
#include "engine_context.h"
#include "render/core/renderer.h"
#include "thumbnailer.h"
#include "world/cell_streaming.h"

namespace rec {
namespace {

// Config overrides, populated from the environment by
// base::InitOptionsFromEnv() at startup.
base::Option<bool> EditorDemo{"editor.demo", false, "REC_EDITOR_DEMO"};
base::Option<bool> Cam{"cam", false, "REC_CAM"};

constexpr f32 kFovY = 1.0472f;           // matches CameraPose::fov_y (60 degrees)
constexpr f32 kStatusSeconds = 3.5f;     // how long a status message lingers
constexpr f32 kRotateStep = 0.2617994f;  // 15 degrees per rotate press
constexpr f32 kScaleStep = 1.08f;        // wheel scale factor per notch
constexpr f32 kAimMaxDist = 250.0f;      // ground-ray march distance (metres)
constexpr f32 kFallbackDist = 12.0f;     // place distance when no ground is hit
constexpr f32 kIdentityRot[4] = {0, 0, 0, 1};
constexpr f32 kUnitsToMeters = 0.01428f;  // mirrors CellStreamer; scale display

// Grid snap sizes cycled by the status-bar grid chip.
constexpr f32 kGridSizes[] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
const char* const kGridLabels[] = {"0.5 m", "1 m", "2 m", "4 m", "8 m"};
constexpr int kGridCount = 5;

// The scene-tree group label for a catalog category (0 / unknown -> "Objects").
std::string GroupName(int category) {
  if (category >= 1 && category < kEditorCategoryCount) return kEditorCategories[category];
  return "Objects";
}

}  // namespace

MapEditor::MapEditor(EngineContext& ctx) : ctx_(ctx) {
  // Route editor-widget clicks back here. The UI is a pure view; all the logic
  // stays in MapEditor.
  if (ctx_.game_ui)
    ctx_.game_ui->SetEditorEventSink([this](const EditorUiEvent& e) { HandleUiEvent(e); });
  for (bool& g : group_expanded_) g = true;  // scene tree starts fully expanded
}

void MapEditor::Toggle() {
  active_ = !active_;
  if (active_) {
    if (!catalog_built_) BuildCatalog();
    ctx_.walk_mode = false;  // editor flies; never walks
    SetStatus("Editor on. Pick an asset, aim, click to place. F4 exits.");
    // Restore a previously saved building the first time the editor opens.
    if (!layout_loaded_) {
      layout_loaded_ = true;
      LoadLayout();
      if (EditorDemo) PlaceDemoBuild();
    }
  } else {
    selected_.clear();
    moving_ = false;
    brush_ = -1;
    prefab_armed_ = false;
    ClearGhost();
  }
  REC_INFO("map editor {}", active_ ? "on" : "off");
  // Push one view so the overlay shows/hides immediately.
  if (ctx_.game_ui) PushView();
}

void MapEditor::SetStatus(std::string message) {
  status_ = std::move(message);
  status_age_ = 0;
}

void MapEditor::SetPlaceDomains(std::vector<EditorPlaceDomain> domains) {
  domains_ = std::move(domains);
  catalog_built_ = false;  // rebuild against the new domain set next time it opens
}

void MapEditor::EnsureDomains() {
  if (!domains_.empty()) return;
  domains_.push_back({"Game", "primary", ctx_.records, ctx_.strings, ctx_.streamer});
}

world::CellStreamer* MapEditor::StreamerFor(int domain) const {
  if (domain >= 0 && domain < static_cast<int>(domains_.size()) && domains_[domain].streamer)
    return domains_[domain].streamer;
  return ctx_.streamer;
}

const MapEditor::LightParams* MapEditor::LightFor(bethesda::GlobalFormId base, int domain) const {
  const u64 key = base.packed();
  if (auto it = light_cache_.find(key); it != light_cache_.end())
    return it->second ? &*it->second : nullptr;

  std::optional<LightParams> result;
  if (domain >= 0 && domain < static_cast<int>(domains_.size()) && domains_[domain].records) {
    bethesda::RecordStore& records = *domains_[domain].records;
    const bethesda::RecordStore::StoredRecord* stored = records.Find(base);
    bethesda::Record record;
    if (stored && stored->header.type == FourCc('L', 'I', 'G', 'H') &&
        records.Parse(base, &record)) {
      LightParams lp;
      // LIGH DATA: time(i32) radius(u32 @4, game units) colour(rgba @8) ...
      if (const bethesda::Subrecord* d = record.Find(FourCc('D', 'A', 'T', 'A'));
          d && d->data.size() >= 12) {
        u32 radius_units;
        std::memcpy(&radius_units, d->data.data() + 4, 4);
        if (radius_units > 0 && radius_units < 20000) lp.radius = radius_units * kUnitsToMeters;
        const u8* c = d->data.data() + 8;
        lp.color[0] = c[0] / 255.0f;
        lp.color[1] = c[1] / 255.0f;
        lp.color[2] = c[2] / 255.0f;
        if (lp.color[0] + lp.color[1] + lp.color[2] < 0.05f)  // a black record reads as warm
          lp = LightParams{};
      }
      result = lp;
    }
  }
  auto [it, _] = light_cache_.emplace(key, result);
  return it->second ? &*it->second : nullptr;
}

void MapEditor::CollectLights(base::Vector<render::PointLight>& out) const {
  for (const PlacedObject& p : placed_) {
    const LightParams* lp = LightFor(p.base, p.domain);
    if (!lp) continue;
    const world::Transform* t = ctx_.world->Get<world::Transform>(p.entity);
    if (!t) continue;
    render::PointLight l;
    l.pos_radius[0] = t->position[0];
    l.pos_radius[1] = t->position[1];
    l.pos_radius[2] = t->position[2];
    l.pos_radius[3] = lp->radius;
    l.color_intensity[0] = lp->color[0];
    l.color_intensity[1] = lp->color[1];
    l.color_intensity[2] = lp->color[2];
    l.color_intensity[3] = lp->intensity;
    out.push_back(l);
  }
}

void MapEditor::Update(const InputState& input, f32 dt, bool allow_input) {
  if (!active_) return;
  status_age_ += dt;

  if (allow_input && (search_focused_ || scene_search_focused_)) {
    UpdateSearchInput(input);
    PushView();
    return;
  }

  if (allow_input) {
    const bool lmb = input.button(MouseButton::kLeft);
    const bool click = lmb && !prev_lmb_ && !PointerOverUi(input) && !ctx_.camera->looking();

    // A move gesture rigidly drags the whole selection so it follows the aim
    // point until a click confirms it (or Esc cancels and restores).
    if (moving_) {
      Vec3 aim;
      if (AimPoint(input, &aim)) {
        const Vec3 delta{aim.x - move_pivot_.x, aim.y - move_pivot_.y, aim.z - move_pivot_.z};
        for (size_t i = 0; i < selected_.size() && i < move_origins_.size(); ++i) {
          if (world::Transform* t = ctx_.world->Get<world::Transform>(selected_[i])) {
            t->position[0] = move_origins_[i].position[0] + delta.x;
            t->position[1] = move_origins_[i].position[1] + delta.y;
            t->position[2] = move_origins_[i].position[2] + delta.z;
          }
        }
      }
      if (input.key_pressed(Key::kEscape)) {
        for (size_t i = 0; i < selected_.size() && i < move_origins_.size(); ++i)
          if (world::Transform* t = ctx_.world->Get<world::Transform>(selected_[i]))
            *t = move_origins_[i];
        moving_ = false;
        SetStatus("Move cancelled");
      } else if (click) {
        moving_ = false;
        SetStatus("Moved");
      }
    } else if (prefab_armed_ && lmb && !prev_lmb_ && !PointerOverUi(input) &&
               !ctx_.camera->looking()) {
      // A click stamps the whole prefab at the aim point.
      Vec3 aim;
      if (AimPoint(input, &aim)) StampPrefab(aim);
    } else if (brush_ >= 0 && lmb && !PointerOverUi(input) && !ctx_.camera->looking()) {
      // With a brush armed: the first click drops one; holding and dragging
      // paint-scatters a copy every scatter_spacing_ metres (fast forests).
      Vec3 aim;
      if (!prev_lmb_) {
        PlaceBrush(input);
      } else if (AimPoint(input, &aim)) {
        const f32 dx = aim.x - last_scatter_pos_.x, dz = aim.z - last_scatter_pos_.z;
        if (dx * dx + dz * dz >= scatter_spacing_ * scatter_spacing_) {
          if (PlaceArmedAt(aim, ScatterYaw()) != ecs::kInvalidEntity) {
            last_scatter_pos_ = aim;
            SetStatus("Scattering " + catalog_[brush_].name);
          }
        }
      }
    } else {
      // No brush armed: a click selects what's under the cursor; a click-drag in
      // empty space sweeps a marquee box and selects everything inside it.
      const bool over = PointerOverUi(input);
      const bool look = ctx_.camera->looking();
      if (lmb && !prev_lmb_ && !over && !look) {
        marquee_dragging_ = true;
        marquee_x0_ = marquee_x1_ = input.mouse_x;
        marquee_y0_ = marquee_y1_ = input.mouse_y;
      } else if (lmb && marquee_dragging_) {
        marquee_x1_ = input.mouse_x;
        marquee_y1_ = input.mouse_y;
      } else if (!lmb && marquee_dragging_) {
        marquee_dragging_ = false;
        const f32 ddx = marquee_x1_ - marquee_x0_, ddy = marquee_y1_ - marquee_y0_;
        const bool additive = input.key(Key::kLeftShift);
        if (ddx * ddx + ddy * ddy < 25.0f)
          SelectUnderCursor(input, additive);  // it was a click, not a drag
        else
          BoxSelect(marquee_x0_, marquee_y0_, marquee_x1_, marquee_y1_, additive);
      }
    }
    ApplyKeyboard(input);
    UpdateGhost(input);
    prev_lmb_ = lmb;
  }

  PushView();
}

void MapEditor::UpdateSearchInput(const InputState& input) {
  // Edit whichever search box has focus: the asset filter or the scene tree.
  const bool scene = scene_search_focused_;
  std::string& buf = scene ? scene_search_ : search_;
  bool changed = false;
  for (u8 i = 0; i < input.text_len; ++i) {
    const char c = input.text[i];
    if (c >= 32 && c < 127) {  // printable ASCII; the catalog is romanized
      buf.push_back(c);
      changed = true;
    }
  }
  if (input.key_pressed(Key::kBackspace) && !buf.empty()) {
    buf.pop_back();
    changed = true;
  }
  if (input.key_pressed(Key::kReturn) || input.key_pressed(Key::kEscape)) {
    search_focused_ = false;
    scene_search_focused_ = false;
  }
  // A click in the viewport commits the search and unfocuses.
  const bool lmb = input.button(MouseButton::kLeft);
  if (lmb && !prev_lmb_ && !PointerOverUi(input)) {
    search_focused_ = false;
    scene_search_focused_ = false;
  }
  prev_lmb_ = lmb;
  if (changed && !scene) {
    page_first_ = 0;
    RefreshFilter();
  }
}

void MapEditor::ApplyKeyboard(const InputState& input) {
  const bool ctrl = input.key(Key::kLeftCtrl);

  if (input.key_pressed(Key::kZ) && ctrl) {
    if (input.key(Key::kLeftShift))
      Redo();  // Ctrl+Shift+Z
    else
      Undo();  // Ctrl+Z
    return;
  }
  if (input.key_pressed(Key::kF5)) {
    SaveLayout();
    return;
  }
  if (input.key_pressed(Key::kB)) {
    snap_ = !snap_;
    SetStatus(snap_ ? "Grid snap ON (1 m)" : "Grid snap OFF");
  }
  PruneDeadSelection();

  if (input.key_pressed(Key::kEscape)) {
    if (brush_ >= 0) {
      brush_ = -1;
      SetStatus("Brush cleared");
    } else if (prefab_armed_) {
      prefab_armed_ = false;
      SetStatus("Prefab cleared");
    }
  }

  // While a brush is armed, R orients the next placement instead of an existing
  // selection, so a building can be laid out facing the right way.
  if (brush_ >= 0) {
    if (input.key_pressed(Key::kR)) {
      brush_yaw_ += input.key(Key::kLeftShift) ? -kRotateStep : kRotateStep;
      char buf[48];
      std::snprintf(buf, sizeof(buf), "Brush yaw %.0f deg", brush_yaw_ * 57.29578f);
      SetStatus(buf);
    }
    return;
  }

  if (selected_.empty()) return;

  if (input.key_pressed(Key::kV) && ctrl) DuplicateSelection();
  if (input.key_pressed(Key::kX) || input.key_pressed(Key::kDelete)) DeleteSelection();
  if (input.key_pressed(Key::kG)) {
    if (ctrl)
      SaveSelectionAsPrefab();  // Ctrl+G groups the selection into a prefab
    else if (moving_)
      moving_ = false;  // toggle off; a click would also confirm
    else
      BeginMove();
  }
  if (input.key_pressed(Key::kR))
    RotateSelection(input.key(Key::kLeftShift) ? -kRotateStep : kRotateStep);
  // Wheel scales the selection when not navigating (the fly camera only consumes
  // the wheel while looking, so the two never collide).
  if (input.wheel != 0.0f && !ctx_.camera->looking() && !moving_)
    ScaleSelection(std::pow(kScaleStep, input.wheel));
}

void MapEditor::ArmBrush(int catalog_index) {
  if (catalog_index < 0 || catalog_index >= static_cast<int>(catalog_.size())) return;
  brush_ = catalog_index;
  moving_ = false;
  marquee_dragging_ = false;
  SetStatus("Placing: " + catalog_[brush_].name + "  (click to drop, Esc to clear)");
}

ecs::Entity MapEditor::PlaceArmedAt(const Vec3& pos, f32 yaw) {
  if (brush_ < 0 || !ctx_.world) return ecs::kInvalidEntity;
  const CatalogEntry& e = catalog_[brush_];
  world::CellStreamer* streamer = StreamerFor(e.domain);
  if (!streamer) return ecs::kInvalidEntity;
  const Quat yq = QuatFromAxisAngle({0, 1, 0}, yaw);
  const f32 rot[4] = {yq.x, yq.y, yq.z, yq.w};
  ecs::Entity entity = streamer->PlaceObject(*ctx_.world, e.base, pos, rot, 1.0f);
  if (entity == ecs::kInvalidEntity) return ecs::kInvalidEntity;
  placed_.push_back({entity, e.base, e.name, e.domain, e.category, e.type, e.editor_id});
  PushEdit({UndoKind::kPlace, entity, e.base, {}, e.name, e.domain});
  return entity;
}

f32 MapEditor::ScatterYaw() {
  // A deterministic spread so a dragged forest does not look stamped, without
  // touching a global RNG.
  const u32 h = (++scatter_count_) * 2654435761u;
  return static_cast<f32>(h % 360u) * 0.01745329f;
}

void MapEditor::PlaceBrush(const InputState& input) {
  if (brush_ < 0 || !ctx_.world) return;
  Vec3 pos;
  if (!AimPoint(input, &pos)) return;
  // Drop it facing the brush yaw the user dialed in with R.
  ecs::Entity entity = PlaceArmedAt(pos, brush_yaw_);
  if (entity == ecs::kInvalidEntity) {
    SetStatus("Could not load a model for " + catalog_[brush_].name);
    return;
  }
  selected_ = {entity};
  last_scatter_pos_ = pos;
  SetStatus("Placed " + catalog_[brush_].name);
}

void MapEditor::PlaceDemoBuild() {
  if (!ctx_.streamer || !ctx_.world || catalog_.empty()) return;
  EnsureDomains();
  // One row per loaded game, on the ground ahead of the camera, so a multi-game
  // session (--add-game) shows Skyrim and Fallout 4 props side by side in the
  // same world. Each row steps further along the forward axis.
  const Vec3 eye = ctx_.camera->position();
  const Vec3 fwd = ctx_.camera->forward();
  const Vec3 right = Normalize(Cross(fwd, {0, 1, 0}));
  const Vec3 fwd_h = Normalize(Vec3{fwd.x, 0, fwd.z});
  const Vec3 base_center = eye + fwd_h * 9.0f;
  const int kWant = 8;
  int total = 0;
  for (int d = 0; d < static_cast<int>(domains_.size()); ++d) {
    world::CellStreamer* streamer = StreamerFor(d);
    if (!streamer) continue;
    const Vec3 center = base_center + fwd_h * (static_cast<f32>(d) * 4.0f);
    int placed = 0;
    for (size_t i = 0; i < catalog_.size() && placed < kWant; i += 17) {
      const CatalogEntry& e = catalog_[i];
      if (e.domain != d) continue;
      Vec3 pos = center + right * (static_cast<f32>(placed) - (kWant - 1) * 0.5f) * 3.0f;
      f32 ground = 0;
      if (ctx_.streamer->GroundHeight(pos.x, pos.z, &ground)) pos.y = ground;
      asset::AssetId mid;
      ecs::Entity entity =
          streamer->PlaceObject(*ctx_.world, e.base, pos, kIdentityRot, 1.0f, &mid);
      if (entity == ecs::kInvalidEntity) continue;
      // Keep the primary row human-scale (its mesh bounds are in the shared asset
      // db); secondary games are not size-filtered, just shown.
      if (d == 0 && ctx_.assets) {
        if (const asset::Mesh* mesh = ctx_.assets->FindMesh(mid)) {
          const f32 radius = mesh->bounds_radius * kUnitsToMeters;
          if (radius < 0.3f || radius > 2.5f) {
            ctx_.world->Destroy(entity);
            continue;
          }
        }
      }
      placed_.push_back({entity, e.base, e.name, d, e.category, e.type, e.editor_id});
      PushEdit({UndoKind::kPlace, entity, e.base, {}, e.name, d});
      ++placed;
      ++total;
    }
  }

  // Showcase paint-scatter: a quick grove of a primary-game flora/tree asset
  // beside the rows, the way a hold-drag would lay one down.
  int flora = -1;
  for (size_t i = 0; i < catalog_.size(); ++i) {
    if (catalog_[i].domain == 0 && catalog_[i].category == 5) {
      flora = static_cast<int>(i);
      break;
    }
  }
  if (flora >= 0) {
    brush_ = flora;
    const Vec3 grove = base_center + right * -13.0f;
    for (int gx = 0; gx < 4; ++gx) {
      for (int gz = 0; gz < 4; ++gz) {
        Vec3 p = grove + right * (static_cast<f32>(gx) * 2.2f) +
                 fwd_h * (static_cast<f32>(gz - 1) * 2.2f);
        f32 g = 0;
        if (ctx_.streamer->GroundHeight(p.x, p.z, &g)) p.y = g;
        if (PlaceArmedAt(p, ScatterYaw()) != ecs::kInvalidEntity) ++total;
      }
    }
    brush_ = -1;
  }

  // Showcase light placement: a short line of primary-game LIGH props that emit
  // (CollectLights turns each into a dynamic point light).
  int light = -1;
  for (size_t i = 0; i < catalog_.size(); ++i) {
    if (catalog_[i].domain == 0 && catalog_[i].category == 6) {
      light = static_cast<int>(i);
      break;
    }
  }
  if (light >= 0) {
    brush_ = light;
    const Vec3 line = base_center + right * 13.0f;
    for (int i = 0; i < 4; ++i) {
      Vec3 p = line + fwd_h * (static_cast<f32>(i - 1) * 2.5f);
      f32 g = 0;
      if (ctx_.streamer->GroundHeight(p.x, p.z, &g)) p.y = g + 1.0f;  // lift lamps off the ground
      if (PlaceArmedAt(p, 0.0f) != ecs::kInvalidEntity) ++total;
    }
    brush_ = -1;
  }

  // Select the middle object so the selection reticle and inspector are live.
  if (!placed_.empty()) selected_ = {placed_[placed_.size() / 2].entity};

  // Frame the rows for a clean capture (unless REC_CAM already pinned a vantage):
  // a near-top-down vantage so dense start cells never occlude them.
  if (!Cam) {
    Vec3 row_center = base_center + fwd_h * 2.0f;
    f32 cg = 0;
    if (ctx_.streamer->GroundHeight(row_center.x, row_center.z, &cg)) row_center.y = cg;
    ctx_.camera->set_position(row_center + Vec3{0, 18.0f, 2.0f});
    ctx_.camera->set_yaw_pitch(0.0f, -1.4f);  // look almost straight down, slight tilt
  }

  REC_INFO("editor: demo build placed {} objects across {} game(s)", total, domains_.size());
  SetStatus("Demo build: placed " + std::to_string(total) + " objects");
  SaveLayout();
}

void MapEditor::UpdateGhost(const InputState& input) {
  world::CellStreamer* streamer = brush_ >= 0 ? StreamerFor(catalog_[brush_].domain) : nullptr;
  const bool want =
      brush_ >= 0 && !moving_ && streamer && !PointerOverUi(input) && !ctx_.camera->looking();
  if (!want) {
    ClearGhost();
    return;
  }
  Vec3 aim;
  if (!AimPoint(input, &aim)) {
    ClearGhost();
    return;
  }
  const Quat yq = QuatFromAxisAngle({0, 1, 0}, brush_yaw_);
  if (ghost_entity_ == ecs::kInvalidEntity || ghost_brush_ != brush_ ||
      !ctx_.world->IsAlive(ghost_entity_)) {
    ClearGhost();
    const f32 rot[4] = {yq.x, yq.y, yq.z, yq.w};
    ghost_entity_ = streamer->PlaceObject(*ctx_.world, catalog_[brush_].base, aim, rot, 1.0f);
    ghost_brush_ = brush_;
  } else if (world::Transform* t = ctx_.world->Get<world::Transform>(ghost_entity_)) {
    t->position[0] = aim.x;
    t->position[1] = aim.y;
    t->position[2] = aim.z;
    t->rotation[0] = yq.x;
    t->rotation[1] = yq.y;
    t->rotation[2] = yq.z;
    t->rotation[3] = yq.w;
  }
}

void MapEditor::ClearGhost() {
  if (ghost_entity_ != ecs::kInvalidEntity && ctx_.world->IsAlive(ghost_entity_))
    ctx_.world->Destroy(ghost_entity_);
  ghost_entity_ = ecs::kInvalidEntity;
  ghost_brush_ = -1;
}

void MapEditor::SelectUnderCursor(const InputState& input, bool additive) {
  f32 t = 0;
  ecs::Entity hit = PickEntity(input, &t);
  moving_ = false;
  if (hit == ecs::kInvalidEntity) {
    if (!additive) selected_.clear();  // a plain click on empty space deselects
    SetStatus("Nothing under the cursor");
    return;
  }
  if (additive) {
    // Shift-click toggles the object in the set, building a multi-selection.
    auto it = std::find(selected_.begin(), selected_.end(), hit);
    if (it != selected_.end())
      selected_.erase(it);
    else
      selected_.push_back(hit);
  } else {
    selected_ = {hit};
  }
  const size_t n = selected_.size();
  SetStatus(n <= 1 ? "Selected" : std::to_string(n) + " selected");
}

void MapEditor::DeleteSelection() {
  PruneDeadSelection();
  if (selected_.empty()) return;
  int deleted = 0;
  for (ecs::Entity e : selected_) {
    if (!ctx_.world->IsAlive(e)) continue;
    UndoOp op;
    op.kind = UndoKind::kDelete;
    op.entity = e;
    if (const world::Transform* t = ctx_.world->Get<world::Transform>(e)) op.transform = *t;
    op.base = bethesda::GlobalFormId{0xffff, 0};
    op.name = "object";
    if (const int pi = FindPlaced(e); pi >= 0) {
      op.base = placed_[pi].base;
      op.name = placed_[pi].name;
      op.domain = placed_[pi].domain;
    }
    PushEdit(op);
    ctx_.world->Destroy(e);
    ++deleted;
  }
  placed_.erase(
      std::remove_if(placed_.begin(), placed_.end(),
                     [&](const PlacedObject& p) { return !ctx_.world->IsAlive(p.entity); }),
      placed_.end());
  selected_.clear();
  SetStatus("Deleted " + std::to_string(deleted));
}

void MapEditor::DuplicateSelection() {
  PruneDeadSelection();
  if (selected_.empty()) return;
  std::vector<ecs::Entity> copies;
  for (ecs::Entity e : selected_) {
    const world::Transform* t = ctx_.world->Get<world::Transform>(e);
    const int pi = FindPlaced(e);
    if (!t || pi < 0) continue;  // only editor-owned objects can be duplicated
    const PlacedObject src = placed_[pi];
    world::CellStreamer* streamer = StreamerFor(src.domain);
    if (!streamer) continue;
    const f32 user_scale = t->scale / kUnitsToMeters;  // strip the unit->metre factor
    Vec3 pos{t->position[0] + 1.0f, t->position[1], t->position[2] + 1.0f};
    ecs::Entity copy = streamer->PlaceObject(*ctx_.world, src.base, pos, t->rotation, user_scale);
    if (copy == ecs::kInvalidEntity) continue;
    placed_.push_back({copy, src.base, src.name, src.domain, src.category, src.type, src.editor_id});
    PushEdit({UndoKind::kPlace, copy, src.base, {}, src.name, src.domain});
    copies.push_back(copy);
  }
  if (copies.empty()) {
    SetStatus("Only placed assets can be duplicated");
    return;
  }
  selected_ = std::move(copies);  // select the new copies, ready to drag away
  SetStatus("Duplicated " + std::to_string(selected_.size()));
}

void MapEditor::RotateSelection(f32 radians) {
  PruneDeadSelection();
  const Quat yaw = QuatFromAxisAngle({0, 1, 0}, radians);
  for (ecs::Entity e : selected_) {
    RecordTransform(e);
    world::Transform* t = ctx_.world->Get<world::Transform>(e);
    if (!t) continue;
    Quat cur{t->rotation[0], t->rotation[1], t->rotation[2], t->rotation[3]};
    Quat nr = Normalize(yaw * cur);
    t->rotation[0] = nr.x;
    t->rotation[1] = nr.y;
    t->rotation[2] = nr.z;
    t->rotation[3] = nr.w;
  }
}

void MapEditor::ScaleSelection(f32 factor) {
  PruneDeadSelection();
  for (ecs::Entity e : selected_) {
    RecordTransform(e);
    world::Transform* t = ctx_.world->Get<world::Transform>(e);
    if (!t) continue;
    t->scale = std::clamp(t->scale * factor, kUnitsToMeters * 0.05f, kUnitsToMeters * 50.0f);
  }
}

void MapEditor::BeginMove() {
  PruneDeadSelection();
  if (selected_.empty()) return;
  moving_ = true;
  marquee_dragging_ = false;
  move_origins_.clear();
  for (ecs::Entity e : selected_) {
    RecordTransform(e);
    world::Transform snapshot{};
    if (const world::Transform* t = ctx_.world->Get<world::Transform>(e)) snapshot = *t;
    move_origins_.push_back(snapshot);
  }
  if (const world::Transform* pt = ctx_.world->Get<world::Transform>(Primary()))
    move_pivot_ = Vec3{pt->position[0], pt->position[1], pt->position[2]};
  SetStatus("Move: aim and click to drop, Esc to cancel");
}

void MapEditor::SaveSelectionAsPrefab() {
  PruneDeadSelection();
  const world::Transform* anchor_t = ctx_.world->Get<world::Transform>(Primary());
  if (!anchor_t) return;
  const Vec3 anchor{anchor_t->position[0], anchor_t->position[1], anchor_t->position[2]};
  prefab_.clear();
  for (ecs::Entity e : selected_) {
    const int pi = FindPlaced(e);  // only editor-owned objects can be grouped
    const world::Transform* t = ctx_.world->Get<world::Transform>(e);
    if (pi < 0 || !t) continue;
    PrefabMember m;
    m.base = placed_[pi].base;
    m.domain = placed_[pi].domain;
    m.rel[0] = t->position[0] - anchor.x;
    m.rel[1] = t->position[1] - anchor.y;
    m.rel[2] = t->position[2] - anchor.z;
    for (int i = 0; i < 4; ++i) m.rot[i] = t->rotation[i];
    m.scale = t->scale / kUnitsToMeters;  // strip the unit->metre factor
    prefab_.push_back(m);
  }
  if (prefab_.empty()) {
    SetStatus("Select placed objects to group");
    return;
  }
  prefab_armed_ = true;
  brush_ = -1;  // stamping a prefab replaces the single-asset brush
  ClearGhost();
  SetStatus("Prefab saved (" + std::to_string(prefab_.size()) +
            " parts). Click to stamp, Esc to clear.");
}

void MapEditor::StampPrefab(const Vec3& at) {
  if (prefab_.empty()) return;
  std::vector<ecs::Entity> stamped;
  for (const PrefabMember& m : prefab_) {
    world::CellStreamer* streamer = StreamerFor(m.domain);
    if (!streamer) continue;
    const Vec3 pos{at.x + m.rel[0], at.y + m.rel[1], at.z + m.rel[2]};
    ecs::Entity e = streamer->PlaceObject(*ctx_.world, m.base, pos, m.rot, m.scale);
    if (e == ecs::kInvalidEntity) continue;
    placed_.push_back({e, m.base, "prefab part", m.domain});
    PushEdit({UndoKind::kPlace, e, m.base, {}, "prefab part", m.domain});
    stamped.push_back(e);
  }
  if (!stamped.empty()) {
    selected_ = std::move(stamped);  // select the new copies
    SetStatus("Stamped prefab (" + std::to_string(selected_.size()) + " parts)");
  }
}

MapEditor::UndoOp MapEditor::ApplyAndInvert(const UndoOp& op) {
  UndoOp inv = op;
  switch (op.kind) {
    case UndoKind::kPlace: {
      // Undo a placement by destroying it; the inverse re-creates it (kDelete).
      inv.kind = UndoKind::kDelete;
      if (const world::Transform* t = ctx_.world->Get<world::Transform>(op.entity))
        inv.transform = *t;
      if (ctx_.world->IsAlive(op.entity)) ctx_.world->Destroy(op.entity);
      placed_.erase(std::remove_if(placed_.begin(), placed_.end(),
                                   [&](const PlacedObject& p) { return p.entity == op.entity; }),
                    placed_.end());
      selected_.erase(std::remove(selected_.begin(), selected_.end(), op.entity), selected_.end());
      break;
    }
    case UndoKind::kDelete: {
      // Re-create from the stored base + transform; the inverse deletes the new
      // entity (kPlace). A streamed ref with no base cannot be recreated.
      world::CellStreamer* streamer = StreamerFor(op.domain);
      ecs::Entity e = ecs::kInvalidEntity;
      if (op.base.plugin != 0xffff && streamer) {
        const f32 user_scale = op.transform.scale / kUnitsToMeters;
        Vec3 pos{op.transform.position[0], op.transform.position[1], op.transform.position[2]};
        e = streamer->PlaceObject(*ctx_.world, op.base, pos, op.transform.rotation, user_scale);
      }
      if (e != ecs::kInvalidEntity) {
        placed_.push_back({e, op.base, op.name, op.domain});
        selected_ = {e};
      }
      inv.kind = UndoKind::kPlace;
      inv.entity = e;
      break;
    }
    case UndoKind::kTransform: {
      // Swap the stored transform with the current one, so undo and redo just
      // bounce between the two states.
      if (world::Transform* t = ctx_.world->Get<world::Transform>(op.entity)) {
        inv.transform = *t;
        *t = op.transform;
      }
      break;
    }
  }
  return inv;
}

void MapEditor::Undo() {
  if (undo_.empty()) {
    SetStatus("Nothing to undo");
    return;
  }
  UndoOp op = undo_.back();
  undo_.pop_back();
  redo_.push_back(ApplyAndInvert(op));
  SetStatus("Undid");
}

void MapEditor::Redo() {
  if (redo_.empty()) {
    SetStatus("Nothing to redo");
    return;
  }
  UndoOp op = redo_.back();
  redo_.pop_back();
  undo_.push_back(ApplyAndInvert(op));
  SetStatus("Redid");
}

void MapEditor::PushEdit(const UndoOp& op) {
  undo_.push_back(op);
  redo_.clear();  // a fresh edit invalidates the redo history
}

void MapEditor::RecordTransform(ecs::Entity entity) {
  if (!ctx_.world->IsAlive(entity)) return;
  const world::Transform* t = ctx_.world->Get<world::Transform>(entity);
  if (!t) return;
  // Coalesce consecutive transform snapshots of the same entity so a long
  // wheel-scale or rotate burst collapses to a single undo step.
  if (!undo_.empty() && undo_.back().kind == UndoKind::kTransform &&
      undo_.back().entity == entity) {
    return;
  }
  UndoOp op;
  op.kind = UndoKind::kTransform;
  op.entity = entity;
  op.transform = *t;
  PushEdit(op);
}

ecs::Entity MapEditor::Primary() const {
  return selected_.empty() ? ecs::kInvalidEntity : selected_.back();
}

void MapEditor::PruneDeadSelection() {
  selected_.erase(std::remove_if(selected_.begin(), selected_.end(),
                                 [&](ecs::Entity e) { return !ctx_.world->IsAlive(e); }),
                  selected_.end());
}

int MapEditor::FindPlaced(ecs::Entity e) const {
  for (int i = 0; i < static_cast<int>(placed_.size()); ++i)
    if (placed_[i].entity == e) return i;
  return -1;
}

world::Transform* MapEditor::SelectedTransform() {
  const ecs::Entity e = Primary();
  if (e == ecs::kInvalidEntity || !ctx_.world->IsAlive(e)) return nullptr;
  return ctx_.world->Get<world::Transform>(e);
}

// --- picking / placement geometry ---

bool MapEditor::PointerOverUi(const InputState& input) const {
  const f32 w = static_cast<f32>(ctx_.renderer->output_width());
  const f32 h = static_cast<f32>(ctx_.renderer->output_height());
  const f32 x = input.mouse_x, y = input.mouse_y;
  if (y < kEdToolbarH) return true;          // toolbar
  if (y > h - kEdStatusH) return true;       // status bar
  if (x < kEdSceneW) return true;            // left scene/assets dock
  if (x > w - kEdInspectorW) return true;    // inspector dock (always shown)
  // Bottom asset-browser dock, between the side docks above the status bar.
  if (y > h - kEdStatusH - kEdBrowserH) return true;
  return false;
}

Vec3 MapEditor::CursorRayDir(const InputState& input) const {
  const Vec3 eye = ctx_.camera->position();
  const Vec3 fwd = ctx_.camera->forward();
  const Vec3 right = Normalize(Cross(fwd, {0, 1, 0}));
  const Vec3 up = Cross(right, fwd);
  const f32 w = static_cast<f32>(ctx_.renderer->output_width());
  const f32 h = static_cast<f32>(ctx_.renderer->output_height());
  const f32 aspect = h > 0 ? w / h : 1.0f;
  const f32 tan_half = std::tan(kFovY * 0.5f);
  const f32 ndc_x = w > 0 ? (2.0f * input.mouse_x / w - 1.0f) : 0.0f;
  const f32 ndc_y = h > 0 ? (1.0f - 2.0f * input.mouse_y / h) : 0.0f;
  Vec3 dir = fwd + right * (ndc_x * aspect * tan_half) + up * (ndc_y * tan_half);
  (void)eye;
  return Normalize(dir);
}

bool MapEditor::AimPoint(const InputState& input, Vec3* out) const {
  const Vec3 eye = ctx_.camera->position();
  const Vec3 dir = CursorRayDir(input);
  // March the cursor ray until it dips below the streamed terrain.
  if (ctx_.streamer) {
    const f32 step = 0.5f;
    Vec3 p = eye;
    for (f32 d = 0; d < kAimMaxDist; d += step) {
      p = eye + dir * d;
      f32 ground = 0;
      if (ctx_.streamer->GroundHeight(p.x, p.z, &ground) && p.y <= ground) {
        *out = Snap(Vec3{p.x, ground, p.z});
        return true;
      }
    }
  }
  // No ground (interior, or aimed at the sky): drop it a fixed distance ahead.
  *out = Snap(eye + dir * kFallbackDist);
  return true;
}

Vec3 MapEditor::Snap(const Vec3& p) const {
  if (!snap_ || snap_grid_ <= 0) return p;
  return Vec3{std::round(p.x / snap_grid_) * snap_grid_, p.y,
              std::round(p.z / snap_grid_) * snap_grid_};
}

ecs::Entity MapEditor::PickEntity(const InputState& input, f32* out_t) const {
  const Vec3 eye = ctx_.camera->position();
  const Vec3 dir = CursorRayDir(input);
  ecs::Entity best = ecs::kInvalidEntity;
  f32 best_t = 1e30f;
  ctx_.world->Each<world::Transform, world::Renderable>(
      [&](ecs::Entity entity, world::Transform& t, world::Renderable& r) {
        if (entity == ghost_entity_) return;  // never select the live preview
        if (ctx_.world->Has<world::Hidden>(entity)) return;
        // World-space bounding sphere from the mesh bounds (object space, scaled).
        Vec3 center{t.position[0], t.position[1], t.position[2]};
        f32 radius = 0.5f;
        if (ctx_.assets) {
          if (const asset::Mesh* mesh = ctx_.assets->FindMesh(r.mesh)) {
            Quat q{t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]};
            Vec3 bc{mesh->bounds_center[0], mesh->bounds_center[1], mesh->bounds_center[2]};
            center += Rotate(q, bc) * t.scale;
            radius = mesh->bounds_radius * t.scale;
          }
        }
        if (radius <= 0) return;
        const Vec3 oc = eye - center;
        const f32 b = Dot(oc, dir);
        const f32 c = Dot(oc, oc) - radius * radius;
        const f32 disc = b * b - c;
        if (disc < 0) return;
        const f32 sq = std::sqrt(disc);
        f32 hit = -b - sq;
        if (hit < 0) hit = -b + sq;
        if (hit < 0 || hit >= best_t) return;
        best_t = hit;
        best = entity;
      });
  if (out_t) *out_t = best_t;
  return best;
}

bool MapEditor::ProjectToScreen(const Vec3& world, f32* sx, f32* sy) const {
  const Vec3 eye = ctx_.camera->position();
  const Vec3 cf = ctx_.camera->forward();
  const Vec3 cr = Normalize(Cross(cf, {0, 1, 0}));
  const Vec3 cu = Cross(cr, cf);
  const Vec3 to = world - eye;
  const f32 zc = Dot(to, cf);
  if (zc <= 0.1f) return false;  // behind the camera
  const f32 w = static_cast<f32>(ctx_.renderer->output_width());
  const f32 h = static_cast<f32>(ctx_.renderer->output_height());
  const f32 aspect = h > 0 ? w / h : 1.0f;
  const f32 tan_half = std::tan(kFovY * 0.5f);
  const f32 ndc_x = (Dot(to, cr) / zc) / (tan_half * aspect);
  const f32 ndc_y = (Dot(to, cu) / zc) / tan_half;
  *sx = (ndc_x * 0.5f + 0.5f) * w;
  *sy = (0.5f - ndc_y * 0.5f) * h;
  return true;
}

void MapEditor::BoxSelect(f32 x0, f32 y0, f32 x1, f32 y1, bool additive) {
  const f32 lo_x = std::min(x0, x1), hi_x = std::max(x0, x1);
  const f32 lo_y = std::min(y0, y1), hi_y = std::max(y0, y1);
  if (!additive) selected_.clear();
  ctx_.world->Each<world::Transform, world::Renderable>(
      [&](ecs::Entity e, world::Transform& t, world::Renderable&) {
        if (e == ghost_entity_ || ctx_.world->Has<world::Hidden>(e)) return;
        f32 sx, sy;
        if (!ProjectToScreen(Vec3{t.position[0], t.position[1], t.position[2]}, &sx, &sy)) return;
        if (sx < lo_x || sx > hi_x || sy < lo_y || sy > hi_y) return;
        if (std::find(selected_.begin(), selected_.end(), e) == selected_.end())
          selected_.push_back(e);
      });
  SetStatus(selected_.empty() ? "Nothing in the box"
                              : std::to_string(selected_.size()) + " selected");
}

void MapEditor::HandleUiEvent(const EditorUiEvent& e) {
  using K = EditorUiEvent::Kind;
  switch (e.kind) {
    case K::kTool:
      switch (e.index) {
        case 0: brush_ = -1; moving_ = false; tool_ = 0; SetStatus("Select mode"); break;
        case 1: tool_ = 1; BeginMove(); break;
        case 2: tool_ = 2; RotateSelection(kRotateStep); break;
        case 3: SetStatus("Terrain sculpting is not available yet"); break;
        case 4: SetStatus("Paint: arm an asset, hold and drag to scatter"); break;
        case 5: Toggle(); break;  // Play leaves the editor and resumes play
        case 6: SaveLayout(); break;
        case 7: Undo(); break;
        case 8: Redo(); break;
        default: break;
      }
      break;
    case K::kGizmo:
      if (e.index >= 0 && e.index < 4) {
        tool_ = e.index;
        if (e.index == 1)
          BeginMove();
        else
          moving_ = false;
      }
      break;
    case K::kLeftTab:
      left_tab_ = e.index;  // 0 scene tree, 1 assets list
      break;
    case K::kTreeSelect: {
      if (e.index < 0 || e.index >= static_cast<int>(tree_targets_.size())) break;
      const TreeNode& n = tree_targets_[e.index];
      if (n.kind == 0) {
        root_expanded_ = !root_expanded_;
      } else if (n.kind == 1) {
        if (n.category >= 0 && n.category < kEditorCategoryCount)
          group_expanded_[n.category] = !group_expanded_[n.category];
      } else if (n.kind == 2 && ctx_.world->IsAlive(n.entity)) {
        selected_ = {n.entity};
        moving_ = false;
      }
      break;
    }
    case K::kTreeExpand: {
      if (e.index < 0 || e.index >= static_cast<int>(tree_targets_.size())) break;
      const TreeNode& n = tree_targets_[e.index];
      if (n.kind == 0)
        root_expanded_ = !root_expanded_;
      else if (n.kind == 1 && n.category >= 0 && n.category < kEditorCategoryCount)
        group_expanded_[n.category] = !group_expanded_[n.category];
      break;
    }
    case K::kTreeEye: {
      if (e.index < 0 || e.index >= static_cast<int>(tree_targets_.size())) break;
      const TreeNode& n = tree_targets_[e.index];
      auto toggle = [&](ecs::Entity ent) {
        if (!ctx_.world->IsAlive(ent)) return;
        if (ctx_.world->Has<world::Hidden>(ent))
          ctx_.world->Remove<world::Hidden>(ent);
        else
          ctx_.world->Add<world::Hidden>(ent, world::Hidden{});
      };
      if (n.kind == 2)
        toggle(n.entity);
      else if (n.kind == 1)
        for (const PlacedObject& p : placed_) { if (p.category == n.category) toggle(p.entity); }
      else
        for (const PlacedObject& p : placed_) toggle(p.entity);
      break;
    }
    case K::kTreeScroll:
      tree_scroll_ = std::max(0, tree_scroll_ + e.index * kEdTreeRows);
      break;
    case K::kCategory:
      if (e.index >= 0 && e.index < kEditorCategoryCount) {
        category_ = e.index;
        page_first_ = 0;
        RefreshFilter();
      }
      break;
    case K::kPickCard: {
      const int idx = page_first_ + e.index;
      if (idx >= 0 && idx < static_cast<int>(filtered_.size())) ArmBrush(filtered_[idx]);
      break;
    }
    case K::kCardScroll:
      page_first_ = std::clamp(page_first_ + e.index * kEdCards, 0,
                               std::max(0, static_cast<int>(filtered_.size()) - 1));
      break;
    case K::kFocusScene:
      scene_search_focused_ = true;
      search_focused_ = false;
      break;
    case K::kClearScene:
      scene_search_.clear();
      scene_search_focused_ = false;
      break;
    case K::kFocusAsset:
      search_focused_ = true;
      scene_search_focused_ = false;
      break;
    case K::kClearAsset:
      search_.clear();
      search_focused_ = false;
      page_first_ = 0;
      RefreshFilter();
      break;
    case K::kSnapToggle:
      snap_ = !snap_;
      SetStatus(snap_ ? "Grid snap ON" : "Grid snap OFF");
      break;
    case K::kGridCycle:
      grid_index_ = (grid_index_ + 1) % kGridCount;
      snap_grid_ = kGridSizes[grid_index_];
      SetStatus(std::string("Grid ") + kGridLabels[grid_index_]);
      break;
  }
}

const asset::Mesh* MapEditor::MeshForCatalog(const CatalogEntry& e) {
  if (e.domain != 0 || !ctx_.assets || !ctx_.records) return nullptr;
  bethesda::Record rec;
  if (!ctx_.records->Parse(e.base, &rec)) return nullptr;
  std::string model = rec.GetString(FourCc('M', 'O', 'D', 'L'));
  if (model.empty()) return nullptr;
  std::string path = asset::NormalizePath(model);
  if (path.rfind("meshes/", 0) != 0) path = "meshes/" + path;
  return ctx_.assets->LoadMesh(path);
}

u64 MapEditor::ThumbTexFor(u64 base_packed) const {
  auto it = thumb_tex_.find(base_packed);
  return it != thumb_tex_.end() ? it->second : 0;
}

void MapEditor::GenerateThumbnails() {
  if (!ctx_.game_ui || !ctx_.renderer) return;
  // First use: stand up the renderer + cache dir (once; never retried on failure).
  if (!thumber_) {
    if (thumber_tried_) return;
    thumber_tried_ = true;
    thumber_ = std::make_unique<Thumbnailer>();
    if (!thumber_->Init(*ctx_.renderer, 128)) {
      thumber_.reset();
      return;
    }
    thumb_dir_ = "thumbs";
    std::error_code ec;
    std::filesystem::create_directories(thumb_dir_, ec);
  }
  if (!thumber_->ready()) return;

  // Render at most a couple per frame; load from disk when a prop was already
  // previewed in a previous session. Keyed by base form id so the asset cards
  // and the inspector's model thumbnail share one cache.
  int budget = 2;
  std::vector<u8> pixels;
  auto ensure = [&](u64 key, auto get_mesh) {
    if (budget <= 0 || thumb_tex_.count(key) || thumb_failed_.count(key)) return;
    char path[80];
    std::snprintf(path, sizeof(path), "%s/%016llx.png", thumb_dir_.c_str(),
                  static_cast<unsigned long long>(key));
    bool ok = thumber_->LoadCached(path, pixels);
    if (!ok) {
      if (const asset::Mesh* mesh = get_mesh(); mesh && thumber_->Render(*mesh, pixels)) {
        thumber_->SaveCached(path, pixels);
        ok = true;
      }
    }
    --budget;
    if (!ok) {
      thumb_failed_.insert(key);
      return;
    }
    const u64 tex = ctx_.game_ui->CreateUiTexture(thumber_->size(), thumber_->size(), pixels.data());
    if (tex)
      thumb_tex_[key] = tex;
    else
      thumb_failed_.insert(key);
  };

  // Visible asset cards (primary game only).
  for (int i = 0; i < kEdCards && budget > 0; ++i) {
    const int idx = page_first_ + i;
    if (idx >= static_cast<int>(filtered_.size())) break;
    const CatalogEntry& e = catalog_[filtered_[idx]];
    if (e.domain != 0) {
      thumb_failed_.insert(e.base.packed());
      continue;
    }
    ensure(e.base.packed(), [&] { return MeshForCatalog(e); });
  }

  // The inspector's model thumbnail for the primary selection.
  if (budget > 0) {
    const ecs::Entity sel = Primary();
    const int pi = sel != ecs::kInvalidEntity ? FindPlaced(sel) : -1;
    if (pi >= 0 && placed_[pi].domain == 0) {
      ensure(placed_[pi].base.packed(), [&]() -> const asset::Mesh* {
        const world::Renderable* r = ctx_.world->Get<world::Renderable>(sel);
        return (r && ctx_.assets) ? ctx_.assets->FindMesh(r->mesh) : nullptr;
      });
    }
  }
}

void MapEditor::PushView() {
  if (!ctx_.game_ui) return;
  EditorView v;
  v.active = active_;
  if (!active_) {
    ctx_.game_ui->SetEditorView(v);
    return;
  }
  GenerateThumbnails();
  auto lower = [](std::string x) {
    for (char& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return x;
  };
  // Long editor ids overrun the fixed tree rows / asset cards; ellipsize them.
  auto clip = [](const std::string& s, size_t n) {
    return s.size() > n ? s.substr(0, n) + "…" : s;
  };

  // Toolbar highlight (Select/Move/Rotate map to buttons 0..2); gizmo bar mirrors
  // the gizmo mode 0..3.
  v.tool = (tool_ <= 2) ? tool_ : -1;
  if (moving_) v.tool = 1;
  v.gizmo = tool_;
  v.left_tab = left_tab_;
  v.scene_search = scene_search_;
  v.scene_search_focused = scene_search_focused_;
  v.asset_search = search_;
  v.asset_search_focused = search_focused_;

  // --- scene hierarchy: root -> non-empty category groups -> object leaves ---
  const std::string needle = scene_search_.empty() ? "" : lower(scene_search_);
  std::vector<EditorView::TreeRow> rows;
  std::vector<TreeNode> flat;
  {
    EditorView::TreeRow r;
    r.depth = 0;
    r.icon = 0;
    r.name = "Scene";
    r.expand = root_expanded_ ? 2 : 1;
    rows.push_back(r);
    flat.push_back({0, -1, ecs::kInvalidEntity});
  }
  if (root_expanded_) {
    for (int cat = 0; cat < kEditorCategoryCount; ++cat) {
      std::vector<const PlacedObject*> objs;
      for (const PlacedObject& p : placed_) {
        if (!ctx_.world->IsAlive(p.entity)) continue;
        const int pc = (p.category >= 1 && p.category < kEditorCategoryCount) ? p.category : 0;
        if (pc != cat) continue;
        if (!needle.empty() && lower(p.name).find(needle) == std::string::npos) continue;
        objs.push_back(&p);
      }
      if (objs.empty()) continue;
      EditorView::TreeRow g;
      g.depth = 1;
      g.icon = 1;
      g.name = GroupName(cat) + "  (" + std::to_string(objs.size()) + ")";
      g.expand = group_expanded_[cat] ? 2 : 1;
      rows.push_back(g);
      flat.push_back({1, cat, ecs::kInvalidEntity});
      if (!group_expanded_[cat]) continue;
      for (const PlacedObject* p : objs) {
        EditorView::TreeRow leaf;
        leaf.depth = 2;
        leaf.icon = (cat == 6) ? 2 : 3;  // lights vs meshes
        leaf.name = clip(p->name, 22);
        leaf.selected =
            std::find(selected_.begin(), selected_.end(), p->entity) != selected_.end();
        leaf.hidden = ctx_.world->Has<world::Hidden>(p->entity);
        rows.push_back(leaf);
        flat.push_back({2, cat, p->entity});
      }
    }
  }
  v.tree_total = static_cast<int>(rows.size());
  tree_scroll_ = std::clamp(tree_scroll_, 0, std::max(0, static_cast<int>(rows.size()) - kEdTreeRows));
  tree_targets_.clear();
  for (int i = 0; i < kEdTreeRows && tree_scroll_ + i < static_cast<int>(rows.size()); ++i) {
    v.tree.push_back(rows[tree_scroll_ + i]);
    tree_targets_.push_back(flat[tree_scroll_ + i]);
  }

  // --- asset-browser tabs and per-category counts (over the asset search) ---
  for (int i = 0; i < kEditorCategoryCount; ++i) v.tabs.push_back(kEditorCategories[i]);
  v.tab = category_;
  {
    const std::string an = search_.empty() ? "" : lower(search_);
    std::vector<int> counts(kEditorCategoryCount, 0);
    for (const CatalogEntry& e : catalog_) {
      if (!an.empty() && lower(e.name).find(an) == std::string::npos &&
          lower(e.editor_id).find(an) == std::string::npos)
        continue;
      ++counts[0];
      if (e.category >= 1 && e.category < kEditorCategoryCount) ++counts[e.category];
    }
    for (int i = 0; i < kEditorCategoryCount; ++i)
      v.cats.push_back({kEditorCategories[i], counts[i], i == category_});
  }

  // --- asset cards (current page of the active filter) ---
  static const u32 kCatColors[kEditorCategoryCount] = {0x6c7bf5ff, 0x8a93a8ff, 0xc98a5aff,
                                                       0x5ab0c9ff, 0xb98ad0ff, 0x57bd6aff,
                                                       0xe8b54aff, 0xb6bcccff, 0xd07a7aff};
  v.card_total = static_cast<int>(filtered_.size());
  v.card_first = page_first_;
  for (int i = 0; i < kEdCards; ++i) {
    const int idx = page_first_ + i;
    if (idx >= static_cast<int>(filtered_.size())) break;
    const CatalogEntry& e = catalog_[filtered_[idx]];
    EditorView::Card c;
    c.name = clip(e.name, 11);
    c.color = (e.category >= 0 && e.category < kEditorCategoryCount) ? kCatColors[e.category]
                                                                     : 0x2a2f3aff;
    c.armed = brush_ == filtered_[idx];
    c.thumb = ThumbTexFor(e.base.packed());
    v.cards.push_back(std::move(c));
  }

  // Marquee box (only once it is an actual drag, not a click).
  const f32 mdx = marquee_x1_ - marquee_x0_, mdy = marquee_y1_ - marquee_y0_;
  v.marquee_active = marquee_dragging_ && (mdx * mdx + mdy * mdy >= 25.0f);
  v.marquee[0] = marquee_x0_;
  v.marquee[1] = marquee_y0_;
  v.marquee[2] = marquee_x1_;
  v.marquee[3] = marquee_y1_;

  // --- inspector (primary selection) ---
  const ecs::Entity sel = Primary();
  if (sel != ecs::kInvalidEntity && ctx_.world->IsAlive(sel)) {
    if (const world::Transform* t = ctx_.world->Get<world::Transform>(sel)) {
      v.has_selection = true;
      for (int a = 0; a < 3; ++a) v.pos[a] = t->position[a];
      Quat q{t->rotation[0], t->rotation[1], t->rotation[2], t->rotation[3]};
      const f32 sinp = std::clamp(2.0f * (q.w * q.x - q.y * q.z), -1.0f, 1.0f);
      v.rot[0] = std::asin(sinp) * 57.29578f;
      v.rot[1] = std::atan2(2.0f * (q.w * q.y + q.z * q.x),
                            1.0f - 2.0f * (q.x * q.x + q.y * q.y)) *
                 57.29578f;
      v.rot[2] = std::atan2(2.0f * (q.w * q.z + q.x * q.y),
                            1.0f - 2.0f * (q.z * q.z + q.x * q.x)) *
                 57.29578f;
      const f32 us = t->scale / kUnitsToMeters;
      v.scale[0] = v.scale[1] = v.scale[2] = us;
      v.sel_count = static_cast<int>(selected_.size());

      v.sel_name = "Object";
      v.model_name = "model";
      v.material_name = "Default_mat";
      const PlacedObject* po = nullptr;
      for (const PlacedObject& p : placed_)
        if (p.entity == sel) {
          po = &p;
          break;
        }
      if (po) {
        v.sel_name = po->name;
        v.model_name = po->editor_id.empty() ? po->name : po->editor_id;
        const char tc[5] = {static_cast<char>(po->type & 0xff),
                            static_cast<char>((po->type >> 8) & 0xff),
                            static_cast<char>((po->type >> 16) & 0xff),
                            static_cast<char>((po->type >> 24) & 0xff), '\0'};
        v.sel_type = po->type ? tc : "";
        v.sel_static = po->category == 1;
        v.material_name = (v.sel_type.empty() ? std::string("Default") : v.sel_type) + "_mat";
        v.model_thumb = ThumbTexFor(po->base.packed());
        v.tags.push_back(GroupName(po->category));
        if (domains_.size() > 1 && po->domain >= 0 && po->domain < static_cast<int>(domains_.size()))
          v.tags.push_back(domains_[po->domain].name);
      } else {
        v.tags.push_back("Object");
      }
      if (selected_.size() > 1) v.sel_name = std::to_string(selected_.size()) + " objects";

      // Project the bounding sphere to the screen for the tracking bracket.
      Vec3 wc{t->position[0], t->position[1], t->position[2]};
      if (ctx_.assets) {
        if (const world::Renderable* rnd = ctx_.world->Get<world::Renderable>(sel)) {
          if (const asset::Mesh* mesh = ctx_.assets->FindMesh(rnd->mesh))
            wc += Rotate(q,
                         {mesh->bounds_center[0], mesh->bounds_center[1], mesh->bounds_center[2]}) *
                  t->scale;
        }
      }
      const Vec3 eye = ctx_.camera->position();
      const Vec3 cf = ctx_.camera->forward();
      const Vec3 cr = Normalize(Cross(cf, {0, 1, 0}));
      const Vec3 cu = Cross(cr, cf);
      const Vec3 to = wc - eye;
      const f32 zc = Dot(to, cf);
      if (zc > 0.1f) {
        const f32 w = static_cast<f32>(ctx_.renderer->output_width());
        const f32 h = static_cast<f32>(ctx_.renderer->output_height());
        const f32 aspect = h > 0 ? w / h : 1.0f;
        const f32 tan_half = std::tan(kFovY * 0.5f);
        const f32 ndc_x = (Dot(to, cr) / zc) / (tan_half * aspect);
        const f32 ndc_y = (Dot(to, cu) / zc) / tan_half;
        if (std::fabs(ndc_x) <= 1.3f && std::fabs(ndc_y) <= 1.3f) {
          v.sel_on_screen = true;
          v.sel_screen[0] = (ndc_x * 0.5f + 0.5f) * w;
          v.sel_screen[1] = (0.5f - ndc_y * 0.5f) * h;
        }
      }
    }
  }

  // --- status bar ---
  v.object_count = static_cast<int>(placed_.size());
  v.snapping = snap_;
  v.grid_label = kGridLabels[grid_index_];
  if (status_age_ < kStatusSeconds) v.status = status_;
  ctx_.game_ui->SetEditorView(v);
}

}  // namespace rec
