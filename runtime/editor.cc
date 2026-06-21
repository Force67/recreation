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
    selection_ = ecs::kInvalidEntity;
    moving_ = false;
    brush_ = -1;
  }
  REC_INFO("map editor {}", active_ ? "on" : "off");
  // Push one view so the overlay shows/hides immediately.
  if (ctx_.game_ui) PushView();
}

void MapEditor::SetStatus(std::string message) {
  status_ = std::move(message);
  status_age_ = 0;
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

    // A move gesture keeps the selection glued to the aim point until a click
    // confirms it (or Esc cancels and restores the original transform).
    if (moving_) {
      Vec3 aim;
      world::Transform* t = SelectedTransform();
      if (t && AimPoint(input, &aim)) {
        t->position[0] = aim.x;
        t->position[1] = aim.y;
        t->position[2] = aim.z;
      }
      if (input.key_pressed(Key::kEscape)) {
        if (t) *t = move_origin_;
        moving_ = false;
        SetStatus("Move cancelled");
      } else if (click) {
        moving_ = false;
        SetStatus("Moved");
      }
    } else if (click) {
      // A click in the viewport places the armed brush, otherwise it selects.
      if (brush_ >= 0)
        PlaceBrush(input);
      else
        SelectUnderCursor(input);
    }
    ApplyKeyboard(input);
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
  if (selection_ != ecs::kInvalidEntity && !ctx_.world->IsAlive(selection_)) {
    selection_ = ecs::kInvalidEntity;  // it was unloaded/destroyed elsewhere
  }

  if (input.key_pressed(Key::kEscape) && brush_ >= 0) {
    brush_ = -1;
    SetStatus("Brush cleared");
  }
  if (selection_ == ecs::kInvalidEntity) return;

  if (input.key_pressed(Key::kV) && ctrl) DuplicateSelection();
  if (input.key_pressed(Key::kX) || input.key_pressed(Key::kDelete)) DeleteSelection();
  if (input.key_pressed(Key::kG)) {
    moving_ = !moving_;
    if (moving_) {
      if (world::Transform* t = SelectedTransform()) {
        move_origin_ = *t;
        RecordTransform(selection_);
      }
      SetStatus("Move: aim and click to drop, Esc to cancel");
    }
  }
  if (input.key_pressed(Key::kR)) {
    RecordTransform(selection_);
    RotateSelection(input.key(Key::kLeftShift) ? -kRotateStep : kRotateStep);
  }
  // Wheel scales the selection when not navigating (the fly camera only consumes
  // the wheel while looking, so the two never collide).
  if (input.wheel != 0.0f && !ctx_.camera->looking() && !moving_) {
    RecordTransform(selection_);
    ScaleSelection(std::pow(kScaleStep, input.wheel));
  }
}

void MapEditor::ArmBrush(int catalog_index) {
  if (catalog_index < 0 || catalog_index >= static_cast<int>(catalog_.size())) return;
  brush_ = catalog_index;
  moving_ = false;
  SetStatus("Placing: " + catalog_[brush_].name + "  (click to drop, Esc to clear)");
}

void MapEditor::PlaceBrush(const InputState& input) {
  if (brush_ < 0 || !ctx_.streamer || !ctx_.world) return;
  const CatalogEntry& e = catalog_[brush_];
  Vec3 pos;
  if (!AimPoint(input, &pos)) return;
  // Face a random-ish yaw derived from the placement count so a forest of the
  // same tree does not look stamped; identity for the first drop is fine too.
  ecs::Entity entity = ctx_.streamer->PlaceObject(*ctx_.world, e.base, pos, kIdentityRot, 1.0f);
  if (entity == ecs::kInvalidEntity) {
    SetStatus("Could not load a model for " + e.name);
    return;
  }
  placed_.push_back({entity, e.base, e.name});
  undo_.push_back({UndoKind::kPlace, entity, e.base, {}, e.name});
  selection_ = entity;
  SetStatus("Placed " + e.name);
}

void MapEditor::PlaceDemoBuild() {
  if (!ctx_.streamer || !ctx_.world || catalog_.empty()) return;
  // A row on the ground a few metres ahead of the camera, spread along its right
  // axis. We step through the catalog so the row shows a variety of models, and
  // skip any whose mesh fails to convert.
  const Vec3 eye = ctx_.camera->position();
  const Vec3 fwd = ctx_.camera->forward();
  const Vec3 right = Normalize(Cross(fwd, {0, 1, 0}));
  const Vec3 center = eye + Vec3{fwd.x, 0, fwd.z} * 9.0f;
  int placed = 0;
  const int kWant = 8;
  for (size_t i = 0; i < catalog_.size() && placed < kWant; i += 17) {
    const CatalogEntry& e = catalog_[i];
    Vec3 pos = center + right * (static_cast<f32>(placed) - (kWant - 1) * 0.5f) * 3.0f;
    f32 ground = 0;
    if (ctx_.streamer->GroundHeight(pos.x, pos.z, &ground)) pos.y = ground;
    asset::AssetId mid;
    ecs::Entity entity =
        ctx_.streamer->PlaceObject(*ctx_.world, e.base, pos, kIdentityRot, 1.0f, &mid);
    if (entity == ecs::kInvalidEntity) continue;
    // Keep the demo human-scale: skip oversized architecture and tiny clutter so
    // the row reads as a tidy line of props.
    f32 radius = 1.0f;
    if (ctx_.assets) {
      if (const asset::Mesh* mesh = ctx_.assets->FindMesh(mid))
        radius = mesh->bounds_radius * kUnitsToMeters;
    }
    if (radius < 0.3f || radius > 2.5f) {
      ctx_.world->Destroy(entity);
      continue;
    }
    placed_.push_back({entity, e.base, e.name});
    undo_.push_back({UndoKind::kPlace, entity, e.base, {}, e.name});
    ++placed;
  }
  // Select the middle object so the selection reticle and inspector are live.
  if (!placed_.empty()) selection_ = placed_[placed_.size() / 2].entity;

  // Frame the row for a clean capture (unless REC_CAM already pinned a vantage):
  // a near-top-down vantage so dense start cells never occlude the row.
  if (!std::getenv("REC_CAM")) {
    Vec3 row_center = center;
    f32 cg = 0;
    if (ctx_.streamer->GroundHeight(row_center.x, row_center.z, &cg)) row_center.y = cg;
    ctx_.camera->set_position(row_center + Vec3{0, 16.0f, 2.0f});
    ctx_.camera->set_yaw_pitch(0.0f, -1.4f);  // look almost straight down, slight tilt
  }

  REC_INFO("editor: demo build placed {} objects", placed);
  SetStatus("Demo build: placed " + std::to_string(placed) + " objects");
  SaveLayout();
}

void MapEditor::SelectUnderCursor(const InputState& input) {
  f32 t = 0;
  ecs::Entity hit = PickEntity(input, &t);
  selection_ = hit;
  moving_ = false;
  if (selection_ == ecs::kInvalidEntity) {
    SetStatus("Nothing under the cursor");
  } else {
    SetStatus("Selected");
  }
}

void MapEditor::DeleteSelection() {
  if (selection_ == ecs::kInvalidEntity || !ctx_.world->IsAlive(selection_)) return;
  // Record enough to undo the delete (base form + where it sat) when we know it;
  // a streamed ref we don't own is removed live but not re-creatable here.
  UndoOp op;
  op.kind = UndoKind::kDelete;
  op.entity = selection_;
  if (const world::Transform* t = ctx_.world->Get<world::Transform>(selection_)) op.transform = *t;
  op.base = bethesda::GlobalFormId{0xffff, 0};
  op.name = "object";
  for (const PlacedObject& p : placed_) {
    if (p.entity == selection_) {
      op.base = p.base;
      op.name = p.name;
      break;
    }
  }
  undo_.push_back(op);

  ctx_.world->Destroy(selection_);
  placed_.erase(std::remove_if(placed_.begin(), placed_.end(),
                               [&](const PlacedObject& p) { return p.entity == selection_; }),
                placed_.end());
  selection_ = ecs::kInvalidEntity;
  SetStatus("Deleted");
}

void MapEditor::DuplicateSelection() {
  if (selection_ == ecs::kInvalidEntity || !ctx_.streamer) return;
  const world::Transform* t = ctx_.world->Get<world::Transform>(selection_);
  if (!t) return;
  // Resolve the base form from our placed list; only editor-owned objects can be
  // duplicated (a streamed ref has no standalone base we track).
  bethesda::GlobalFormId base{0xffff, 0};
  std::string name = "object";
  for (const PlacedObject& p : placed_) {
    if (p.entity == selection_) {
      base = p.base;
      name = p.name;
      break;
    }
  }
  if (base.plugin == 0xffff) {
    SetStatus("Only placed assets can be duplicated");
    return;
  }
  // Native multiplier = stored scale without the unit->metre factor.
  const f32 user_scale = t->scale / kUnitsToMeters;
  Vec3 pos{t->position[0] + 1.0f, t->position[1], t->position[2] + 1.0f};
  ecs::Entity e = ctx_.streamer->PlaceObject(*ctx_.world, base, pos, t->rotation, user_scale);
  if (e == ecs::kInvalidEntity) return;
  placed_.push_back({e, base, name});
  undo_.push_back({UndoKind::kPlace, e, base, {}, name});
  selection_ = e;
  SetStatus("Duplicated " + name);
}

void MapEditor::RotateSelection(f32 radians) {
  world::Transform* t = SelectedTransform();
  if (!t) return;
  Quat cur{t->rotation[0], t->rotation[1], t->rotation[2], t->rotation[3]};
  Quat yaw = QuatFromAxisAngle({0, 1, 0}, radians);
  Quat nr = Normalize(yaw * cur);
  t->rotation[0] = nr.x;
  t->rotation[1] = nr.y;
  t->rotation[2] = nr.z;
  t->rotation[3] = nr.w;
}

void MapEditor::ScaleSelection(f32 factor) {
  world::Transform* t = SelectedTransform();
  if (!t) return;
  t->scale = std::clamp(t->scale * factor, kUnitsToMeters * 0.05f, kUnitsToMeters * 50.0f);
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
      if (selection_ == op.entity) selection_ = ecs::kInvalidEntity;
      SetStatus("Undid place");
      break;
    case UndoKind::kTransform:
      if (ctx_.world->IsAlive(op.entity))
        if (world::Transform* t = ctx_.world->Get<world::Transform>(op.entity)) *t = op.transform;
      SetStatus("Undid transform");
      break;
    case UndoKind::kDelete:
      // Re-place editor-owned deletes from their base form; live streamed refs
      // we cannot recreate just report that.
      if (op.base.plugin != 0xffff && ctx_.streamer) {
        const f32 user_scale = op.transform.scale / kUnitsToMeters;
        Vec3 pos{op.transform.position[0], op.transform.position[1], op.transform.position[2]};
        ecs::Entity e = ctx_.streamer->PlaceObject(*ctx_.world, op.base, pos, op.transform.rotation,
                                                   user_scale);
        if (e != ecs::kInvalidEntity) {
          placed_.push_back({e, op.base, op.name});
          selection_ = e;
          SetStatus("Undid delete");
        }
      } else {
        SetStatus("Cannot restore a streamed object");
      }
      break;
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

world::Transform* MapEditor::SelectedTransform() {
  if (selection_ == ecs::kInvalidEntity || !ctx_.world->IsAlive(selection_)) return nullptr;
  return ctx_.world->Get<world::Transform>(selection_);
}

// --- picking / placement geometry ---

bool MapEditor::PointerOverUi(const InputState& input) const {
  const f32 w = static_cast<f32>(ctx_.renderer->output_width());
  const f32 h = static_cast<f32>(ctx_.renderer->output_height());
  const f32 x = input.mouse_x, y = input.mouse_y;
  if (y < kEditorToolbarHeight) return true;     // toolbar
  if (y > h - kEditorStatusHeight) return true;  // status bar
  if (x < kEditorBrowserWidth) return true;      // asset browser dock
  if (selection_ != ecs::kInvalidEntity && x > w - kEditorInspectorWidth) return true;  // inspector
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
        *out = Vec3{p.x, ground, p.z};
        return true;
      }
    }
  }
  // No ground (interior, or aimed at the sky): drop it a fixed distance ahead.
  *out = eye + dir * kFallbackDist;
  return true;
}

ecs::Entity MapEditor::PickEntity(const InputState& input, f32* out_t) const {
  const Vec3 eye = ctx_.camera->position();
  const Vec3 dir = CursorRayDir(input);
  ecs::Entity best = ecs::kInvalidEntity;
  f32 best_t = 1e30f;
  ctx_.world->Each<world::Transform, world::Renderable>(
      [&](ecs::Entity entity, world::Transform& t, world::Renderable& r) {
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
          if (selection_ != ecs::kInvalidEntity) {
            moving_ = true;
            if (world::Transform* t = SelectedTransform()) {
              move_origin_ = *t;
              RecordTransform(selection_);
            }
            SetStatus("Move: aim and click to drop, Esc to cancel");
          }
          break;
        case kToolRotate:
          if (selection_ != ecs::kInvalidEntity) {
            RecordTransform(selection_);
            RotateSelection(kRotateStep);
          }
          break;
        case kToolScale:
          if (selection_ != ecs::kInvalidEntity) {
            RecordTransform(selection_);
            ScaleSelection(kScaleStep);
          }
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
    std::snprintf(sub, sizeof(sub), "%s  %s", tc, e.editor_id.c_str());
    ar.subtitle = sub;
    ar.armed = brush_ == filtered_[idx];
    v.rows.push_back(std::move(ar));
  }

  if (selection_ != ecs::kInvalidEntity && ctx_.world->IsAlive(selection_)) {
    if (const world::Transform* t = ctx_.world->Get<world::Transform>(selection_)) {
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
        if (p.entity == selection_) {
          v.sel_title = p.name;
          break;
        }
      }

      // Project the object's bounding sphere to the screen so the overlay can
      // draw a tracking bracket around it. Mirrors the picking projection.
      Vec3 wc{t->position[0], t->position[1], t->position[2]};
      f32 wr = 1.0f;
      if (ctx_.assets) {
        if (const world::Renderable* rnd = ctx_.world->Get<world::Renderable>(selection_)) {
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
