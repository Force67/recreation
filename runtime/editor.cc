#include "editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "core/input.h"
#include "core/log.h"
#include "ecs/world.h"
#include "engine_context.h"
#include "render/renderer.h"
#include "world/cell_streaming.h"

namespace rec {
namespace {

constexpr f32 kFovY = 1.0472f;           // matches CameraPose::fov_y (60 degrees)
constexpr f32 kStatusSeconds = 3.5f;     // how long a status message lingers
constexpr f32 kRotateStep = 0.2617994f;  // 15 degrees per rotate press
constexpr f32 kScaleStep = 1.08f;        // wheel scale factor per notch
constexpr f32 kAimMaxDist = 250.0f;      // ground-ray march distance (metres)
constexpr f32 kFallbackDist = 12.0f;     // place distance when no ground is hit
constexpr f32 kIdentityRot[4] = {0, 0, 0, 1};
constexpr f32 kUnitsToMeters = 0.01428f;  // mirrors CellStreamer; scale display

// Toolbar actions, forwarded as EditorUiEvent::Kind::kTool with this index.
enum ToolAction {
  kToolSelect = 0,
  kToolMove = 1,
  kToolRotate = 2,
  kToolScale = 3,
  kToolDelete = 4,
  kToolDuplicate = 5,
  kToolUndo = 6,
  kToolSave = 7,
  kToolFocusSearch = 8,
  kToolClearSearch = 9,
};

}  // namespace

MapEditor::MapEditor(EngineContext& ctx) : ctx_(ctx) {
  // Route editor-widget clicks back here. The UI is a pure view; all the logic
  // stays in MapEditor.
  if (ctx_.game_ui)
    ctx_.game_ui->SetEditorEventSink([this](const EditorUiEvent& e) { HandleUiEvent(e); });
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
      if (std::getenv("REC_EDITOR_DEMO")) PlaceDemoBuild();
    }
  } else {
    selected_.clear();
    moving_ = false;
    brush_ = -1;
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

void MapEditor::Update(const InputState& input, f32 dt, bool allow_input) {
  if (!active_) return;
  status_age_ += dt;

  if (allow_input && search_focused_) {
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
    } else if (click) {
      SelectUnderCursor(input, input.key(Key::kLeftShift));
    }
    ApplyKeyboard(input);
    UpdateGhost(input);
    prev_lmb_ = lmb;
  }

  PushView();
}

void MapEditor::UpdateSearchInput(const InputState& input) {
  bool changed = false;
  for (u8 i = 0; i < input.text_len; ++i) {
    const char c = input.text[i];
    if (c >= 32 && c < 127) {  // printable ASCII; the catalog is romanized
      search_.push_back(c);
      changed = true;
    }
  }
  if (input.key_pressed(Key::kBackspace) && !search_.empty()) {
    search_.pop_back();
    changed = true;
  }
  if (input.key_pressed(Key::kReturn) || input.key_pressed(Key::kEscape)) search_focused_ = false;
  // A click in the viewport commits the search and unfocuses.
  const bool lmb = input.button(MouseButton::kLeft);
  if (lmb && !prev_lmb_ && !PointerOverUi(input)) search_focused_ = false;
  prev_lmb_ = lmb;
  if (changed) {
    page_first_ = 0;
    RefreshFilter();
  }
}

void MapEditor::ApplyKeyboard(const InputState& input) {
  const bool ctrl = input.key(Key::kLeftCtrl);

  if (input.key_pressed(Key::kZ) && ctrl) {
    Undo();
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

  if (input.key_pressed(Key::kEscape) && brush_ >= 0) {
    brush_ = -1;
    SetStatus("Brush cleared");
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
    if (moving_)
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
  placed_.push_back({entity, e.base, e.name, e.domain});
  undo_.push_back({UndoKind::kPlace, entity, e.base, {}, e.name, e.domain});
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
      placed_.push_back({entity, e.base, e.name, d});
      undo_.push_back({UndoKind::kPlace, entity, e.base, {}, e.name, d});
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

  // Select the middle object so the selection reticle and inspector are live.
  if (!placed_.empty()) selected_ = {placed_[placed_.size() / 2].entity};

  // Frame the rows for a clean capture (unless REC_CAM already pinned a vantage):
  // a near-top-down vantage so dense start cells never occlude them.
  if (!std::getenv("REC_CAM")) {
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
    undo_.push_back(op);
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
    placed_.push_back({copy, src.base, src.name, src.domain});
    undo_.push_back({UndoKind::kPlace, copy, src.base, {}, src.name, src.domain});
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

void MapEditor::Undo() {
  if (undo_.empty()) {
    SetStatus("Nothing to undo");
    return;
  }
  UndoOp op = undo_.back();
  undo_.pop_back();
  switch (op.kind) {
    case UndoKind::kPlace:
      if (ctx_.world->IsAlive(op.entity)) ctx_.world->Destroy(op.entity);
      placed_.erase(std::remove_if(placed_.begin(), placed_.end(),
                                   [&](const PlacedObject& p) { return p.entity == op.entity; }),
                    placed_.end());
      selected_.erase(std::remove(selected_.begin(), selected_.end(), op.entity), selected_.end());
      SetStatus("Undid place");
      break;
    case UndoKind::kTransform:
      if (ctx_.world->IsAlive(op.entity))
        if (world::Transform* t = ctx_.world->Get<world::Transform>(op.entity)) *t = op.transform;
      SetStatus("Undid transform");
      break;
    case UndoKind::kDelete: {
      // Re-place editor-owned deletes from their base form; live streamed refs
      // we cannot recreate just report that.
      world::CellStreamer* streamer = StreamerFor(op.domain);
      if (op.base.plugin != 0xffff && streamer) {
        const f32 user_scale = op.transform.scale / kUnitsToMeters;
        Vec3 pos{op.transform.position[0], op.transform.position[1], op.transform.position[2]};
        ecs::Entity e =
            streamer->PlaceObject(*ctx_.world, op.base, pos, op.transform.rotation, user_scale);
        if (e != ecs::kInvalidEntity) {
          placed_.push_back({e, op.base, op.name, op.domain});
          selected_ = {e};
          SetStatus("Undid delete");
        }
      } else {
        SetStatus("Cannot restore a streamed object");
      }
      break;
    }
  }
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
  undo_.push_back(op);
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
  if (y < kEditorToolbarHeight) return true;                             // toolbar
  if (y > h - kEditorStatusHeight) return true;                          // status bar
  if (x < kEditorBrowserWidth) return true;                              // asset browser dock
  if (!selected_.empty() && x > w - kEditorInspectorWidth) return true;  // inspector dock
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

void MapEditor::HandleUiEvent(const EditorUiEvent& e) {
  using Kind = EditorUiEvent::Kind;
  switch (e.kind) {
    case Kind::kPickRow: {
      const int idx = page_first_ + e.index;
      if (idx >= 0 && idx < static_cast<int>(filtered_.size())) ArmBrush(filtered_[idx]);
      break;
    }
    case Kind::kCategory:
      if (e.index >= 0 && e.index < kEditorCategoryCount) {
        category_ = e.index;
        page_first_ = 0;
        RefreshFilter();
      }
      break;
    case Kind::kScroll: {
      const int rows = kEditorBrowserRows;
      page_first_ = std::clamp(page_first_ + e.index * rows, 0,
                               std::max(0, static_cast<int>(filtered_.size()) - 1));
      break;
    }
    case Kind::kCloseBrowser:
      brush_ = -1;
      break;
    case Kind::kTool:
      switch (e.index) {
        case kToolSelect:
          brush_ = -1;
          moving_ = false;
          break;
        case kToolMove:
          BeginMove();
          break;
        case kToolRotate:
          RotateSelection(kRotateStep);
          break;
        case kToolScale:
          ScaleSelection(kScaleStep);
          break;
        case kToolDelete:
          DeleteSelection();
          break;
        case kToolDuplicate:
          DuplicateSelection();
          break;
        case kToolUndo:
          Undo();
          break;
        case kToolSave:
          SaveLayout();
          break;
        case kToolFocusSearch:
          search_focused_ = true;
          break;
        case kToolClearSearch:
          search_.clear();
          search_focused_ = false;
          page_first_ = 0;
          RefreshFilter();
          break;
        default:
          break;
      }
      break;
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
  v.browser_open = true;
  v.tool = moving_ ? 1 : tool_;
  v.category = category_;
  v.search = search_;
  v.search_focused = search_focused_;
  for (int i = 0; i < kEditorCategoryCount; ++i) v.categories.push_back(kEditorCategories[i]);

  v.result_count = static_cast<int>(filtered_.size());
  v.page_first = page_first_;
  for (int row = 0; row < kEditorBrowserRows; ++row) {
    const int idx = page_first_ + row;
    if (idx >= static_cast<int>(filtered_.size())) break;
    const CatalogEntry& e = catalog_[filtered_[idx]];
    EditorView::AssetRow ar;
    ar.name = e.name;
    char sub[96];
    const char tc[5] = {static_cast<char>(e.type & 0xff), static_cast<char>((e.type >> 8) & 0xff),
                        static_cast<char>((e.type >> 16) & 0xff),
                        static_cast<char>((e.type >> 24) & 0xff), '\0'};
    // With more than one game loaded, lead the subtitle with the game so a
    // Fallout 4 prop is obviously distinct from a Skyrim one.
    if (domains_.size() > 1 && e.domain >= 0 && e.domain < static_cast<int>(domains_.size())) {
      std::snprintf(sub, sizeof(sub), "%s  %s  %s", domains_[e.domain].name.c_str(), tc,
                    e.editor_id.c_str());
    } else {
      std::snprintf(sub, sizeof(sub), "%s  %s", tc, e.editor_id.c_str());
    }
    ar.subtitle = sub;
    ar.armed = brush_ == filtered_[idx];
    v.rows.push_back(std::move(ar));
  }

  const ecs::Entity sel = Primary();
  if (sel != ecs::kInvalidEntity && ctx_.world->IsAlive(sel)) {
    if (const world::Transform* t = ctx_.world->Get<world::Transform>(sel)) {
      v.has_selection = true;
      v.sel_pos[0] = t->position[0];
      v.sel_pos[1] = t->position[1];
      v.sel_pos[2] = t->position[2];
      Quat q{t->rotation[0], t->rotation[1], t->rotation[2], t->rotation[3]};
      Vec3 f = Rotate(q, {0, 0, -1});
      f32 yaw = std::atan2(f.x, -f.z) * 57.29578f;
      if (yaw < 0) yaw += 360.0f;
      v.sel_yaw_deg = yaw;
      v.sel_scale = t->scale / kUnitsToMeters;
      v.sel_title = "Selected object";
      v.sel_subtitle = "move G / rotate R / scale wheel / delete X";
      for (const PlacedObject& p : placed_) {
        if (p.entity == sel) {
          v.sel_title = p.name;
          break;
        }
      }
      // A multi-selection leads with the count; the primary's transform shows.
      if (selected_.size() > 1) {
        v.sel_subtitle = v.sel_title + "  (primary)";
        v.sel_title = std::to_string(selected_.size()) + " objects selected";
      }

      // Project the object's bounding sphere to the screen so the overlay can
      // draw a tracking bracket around it. Mirrors the picking projection.
      Vec3 wc{t->position[0], t->position[1], t->position[2]};
      f32 wr = 1.0f;
      if (ctx_.assets) {
        if (const world::Renderable* rnd = ctx_.world->Get<world::Renderable>(sel)) {
          if (const asset::Mesh* mesh = ctx_.assets->FindMesh(rnd->mesh)) {
            wc += Rotate(q,
                         {mesh->bounds_center[0], mesh->bounds_center[1], mesh->bounds_center[2]}) *
                  t->scale;
            wr = mesh->bounds_radius * t->scale;
          }
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
          v.sel_screen_half = (wr / zc) / tan_half * (h * 0.5f) * 1.2f;
        }
      }
    }
  }

  if (brush_ >= 0 && brush_ < static_cast<int>(catalog_.size())) v.brush = catalog_[brush_].name;
  v.object_count = static_cast<int>(placed_.size());
  if (status_age_ < kStatusSeconds) v.status = status_;
  ctx_.game_ui->SetEditorView(v);
}

}  // namespace rec
