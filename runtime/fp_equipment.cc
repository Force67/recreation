#include "fp_equipment.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "actor_system.h"
#include "asset/asset_id.h"
#include "bethesda/record.h"
#include "core/input_actions.h"
#include "core/log.h"
#include "core/types.h"
#include "ecs/world.h"
#include "engine_context.h"
#include "game_input.h"
#include "inventory/components.h"
#include "inventory/inventory.h"
#include "item_bridge.h"
#include "world/cell_streaming.h"
#include "world/components.h"

namespace rx {
namespace {

// Stable game-hashed equipment slot tag for the right hand (rx::inventory treats
// the tag as opaque). FNV-1a of "hand.right" so the same constant round-trips.
constexpr u32 Fnv1a(const char* s) {
  u32 h = 2166136261u;
  for (; *s; ++s) h = (h ^ static_cast<u8>(*s)) * 16777619u;
  return h;
}
constexpr u32 kHandRightTag = Fnv1a("hand.right");
constexpr u32 kWeapType = FourCc('W', 'E', 'A', 'P');

}  // namespace

FpEquipment::FpEquipment(EngineContext& ctx, ActorSystem& actors) : ctx_(ctx), actors_(actors) {}

bool FpEquipment::InputBlocked() const {
  if (ctx_.game_ui && ctx_.game_ui->menu_open()) return true;
  if (ctx_.debug_ui && ctx_.debug_ui->wants_keyboard()) return true;
  return false;
}

void FpEquipment::Update(f32 dt) {
  if (!ctx_.world || !actors_.HasPlayer()) return;
  MaybeRunProbe(dt);

  const ActionState* actions = ctx_.actions;
  const bool blocked = InputBlocked();
  const bool walk = ctx_.walk_mode;
  const bool first_person = walk && !ctx_.third_person;

  // Equip / sheathe toggle (walk mode). Draw when empty-handed, sheathe otherwise.
  if (actions && walk && !blocked && actions->pressed(Action::kEquipWeapon)) {
    if (state_ == State::kSheathed)
      BeginEquip();
    else if (state_ != State::kUnequipping)
      BeginUnequip();
  }

  // Attack swing: the melee key plays a one-shot swing over the held idle, only
  // while a weapon is drawn and the arms are actually on screen (first person).
  if (actions && first_person && !blocked && state_ == State::kDrawn &&
      actions->pressed(Action::kAttack)) {
    actors_.PlayFpClip(ActorSystem::FpClip::kAttack);
    state_ = State::kAttacking;
  }

  // Advance the one-shot clips back into the held idle / sheathed rest.
  switch (state_) {
    case State::kEquipping:
      if (actors_.FpClipDone()) {
        actors_.PlayFpClip(ActorSystem::FpClip::kIdle);
        state_ = State::kDrawn;
      }
      break;
    case State::kAttacking:
      if (actors_.FpClipDone()) {
        actors_.PlayFpClip(ActorSystem::FpClip::kIdle);
        state_ = State::kDrawn;
      }
      break;
    case State::kUnequipping:
      if (actors_.FpClipDone()) {
        actors_.ClearFpWeapon();
        if (inventory::Equipment* eq =
                ctx_.world->Get<inventory::Equipment>(actors_.PlayerEntity()))
          inventory::Unequip(*eq, kHandRightTag);
        equipped_item_ = inventory::kInvalidItemDef;
        state_ = State::kSheathed;
      }
      break;
    default:
      break;
  }

  // A weapon that left the inventory (dropped) forces a sheathe, so we never keep
  // rendering a weapon the player no longer carries.
  if (equipped_item_ != inventory::kInvalidItemDef && state_ != State::kUnequipping &&
      state_ != State::kSheathed) {
    const inventory::Inventory* inv =
        ctx_.world->Get<inventory::Inventory>(actors_.PlayerEntity());
    if (!inv || inventory::InventoryCount(*inv, equipped_item_) == 0) BeginUnequip();
  }

  // Render only while a weapon is engaged and the arms would be on screen; the
  // pose advances whenever the rig is engaged so a hidden (third-person) draw
  // keeps ticking its clip and pops straight back to the idle on return.
  const bool engaged = state_ != State::kSheathed;
  const bool visible = engaged && first_person;
  actors_.SetFpFlags(engaged, visible);
  if (visible) actors_.SetFpRootView(ctx_.walk_eye, ctx_.walk_target);
}

void FpEquipment::BeginEquip() {
  if (!ctx_.world || !ctx_.items) return;
  const ecs::Entity player = actors_.PlayerEntity();
  inventory::Inventory* inv = ctx_.world->Get<inventory::Inventory>(player);
  if (!inv) {
    if (ctx_.game_ui) ctx_.game_ui->FlashQuestUpdate("No weapon to equip");
    return;
  }
  // Most-recent weapon: the last non-empty stack whose def is a WEAP record.
  const inventory::ItemCatalog& catalog = ctx_.items->catalog();
  inventory::ItemDefId weapon = inventory::kInvalidItemDef;
  for (int i = static_cast<int>(inv->entries.size()) - 1; i >= 0; --i) {
    const inventory::InventoryEntry& e = inv->entries[i];
    if (e.count == 0) continue;
    const inventory::ItemDef* def = catalog.Find(e.item);
    if (def && def->flags == kWeapType) {
      weapon = e.item;
      break;
    }
  }
  if (weapon == inventory::kInvalidItemDef) {
    if (ctx_.game_ui) ctx_.game_ui->FlashQuestUpdate("No weapon to equip");
    return;
  }
  if (!actors_.EnsureFpRig()) {
    RX_WARN("fp: first-person rig assets unavailable; cannot draw weapon");
    return;
  }
  // Mark equipped in rx::Equipment (inventory keeps ownership of the stack).
  if (!ctx_.world->Has<inventory::Equipment>(player))
    ctx_.world->Add(player, inventory::Equipment{});
  inventory::Equipment* eq = ctx_.world->Get<inventory::Equipment>(player);
  inventory::Equip(*inv, *eq, kHandRightTag, weapon);

  const inventory::ItemDef* def = catalog.Find(weapon);
  actors_.SetFpWeapon(def ? def->world_mesh : asset::AssetId{});
  actors_.PlayFpClip(ActorSystem::FpClip::kEquip);
  state_ = State::kEquipping;
  equipped_item_ = weapon;
  if (ctx_.game_ui) ctx_.game_ui->FlashQuestUpdate("Weapon drawn");
  RX_INFO("fp: drew weapon (item def {}), playing 1hm_equip", weapon);
}

void FpEquipment::BeginUnequip() {
  actors_.PlayFpClip(ActorSystem::FpClip::kUnequip);
  state_ = State::kUnequipping;
  RX_INFO("fp: sheathing weapon, playing 1hm_unequip");
}

bool FpEquipment::ProbePickupNearestWeapon() {
  if (!ctx_.records || !ctx_.items) return false;
  Vec3 ppos;
  if (!actors_.PlayerWorldPos(&ppos)) return false;
  // Track the nearest weapon overall and, separately, the nearest genuine
  // one-handed sword: the 1hm clips + WEAPON node are authored for a 1h sword, so
  // it sits in the hand and swings on-screen (a greatsword floats and arcs out of
  // frame on the 1h rig). Prefer the sword when the cell has one.
  u64 best = 0, best_sword = 0;
  f32 best_d2 = 1e30f, best_sword_d2 = 1e30f;
  ctx_.world->Each<world::FormLink, world::Transform>(
      [&](ecs::Entity, world::FormLink& link, world::Transform& t) {
        const u64 handle = link.form.packed();
        const bethesda::GlobalFormId refr{static_cast<u16>(handle >> 32),
                                          static_cast<u32>(handle & 0xffffffffu)};
        const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(refr);
        if (!stored) return;
        bethesda::Record rec;
        if (!ctx_.records->Parse(refr, &rec)) return;
        const bethesda::Subrecord* name = rec.Find(FourCc('N', 'A', 'M', 'E'));
        if (!name || name->data.size() < 4) return;
        u32 base_raw;
        std::memcpy(&base_raw, name->data.data(), 4);
        const bethesda::GlobalFormId base =
            ctx_.records->ResolveFrom(bethesda::RawFormId{base_raw}, stored->winning_plugin);
        const bethesda::RecordStore::StoredRecord* bstored = ctx_.records->Find(base);
        if (!bstored || bstored->header.type != kWeapType) return;
        const f32 dx = t.position[0] - ppos.x, dy = t.position[1] - ppos.y,
                  dz = t.position[2] - ppos.z;
        const f32 d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) { best_d2 = d2; best = handle; }
        bethesda::Record brec;
        if (ctx_.records->Parse(base, &brec)) {
          std::string model = brec.GetString(FourCc('M', 'O', 'D', 'L'));
          for (char& c : model) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (model.find("sword") != std::string::npos &&
              model.find("greatsword") == std::string::npos && d2 < best_sword_d2) {
            best_sword_d2 = d2;
            best_sword = handle;
          }
        }
      });
  if (best_sword != 0) { best = best_sword; best_d2 = best_sword_d2; }
  if (best == 0) {
    RX_INFO("FP_PROBE: no loose weapon reference in the loaded cells");
    return false;
  }
  RX_INFO("FP_PROBE: picking up nearest weapon ref 0x{:x} at {:.1f} m", best,
          std::sqrt(best_d2));
  return ctx_.items->TryPickUp(best);
}

void FpEquipment::ProbeForceDraw() {
  if (!actors_.EnsureFpRig() || !ctx_.records || !ctx_.streamer) return;
  // Prefer a genuine one-handed sword (the 1hm clips + the WEAPON node are
  // authored for it); fall back to any WEAP with a hand mesh.
  asset::AssetId sword{};
  asset::AssetId any_mesh{};
  ctx_.records->EachOfType(kWeapType, [&](bethesda::GlobalFormId id,
                                          const bethesda::RecordStore::StoredRecord&) {
    if (sword.hash != 0) return;
    bethesda::Record rec;
    if (!ctx_.records->Parse(id, &rec)) return;
    std::string model = rec.GetString(FourCc('M', 'O', 'D', 'L'));
    for (char& c : model) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    asset::AssetId rid{};
    if (!ctx_.streamer->PrepareItemModel(id, &rid) || rid.hash == 0) return;
    if (any_mesh.hash == 0) any_mesh = rid;
    if (model.find("sword") != std::string::npos &&
        model.find("greatsword") == std::string::npos)
      sword = rid;
  });
  const asset::AssetId mesh = sword.hash != 0 ? sword : any_mesh;
  actors_.SetFpWeapon(mesh);
  actors_.PlayFpClip(ActorSystem::FpClip::kEquip);
  state_ = State::kEquipping;
  RX_INFO("FP_PROBE: no inventory weapon; driving the rig directly ({} mesh {:#x})",
          sword.hash != 0 ? "1h sword" : "fallback", mesh.hash);
}

void FpEquipment::MaybeRunProbe(f32 dt) {
  static const char* kDir = std::getenv("RX_FP_PROBE");
  if (!kDir || probe_phase_ >= 8) return;
  probe_t_ += dt;
  // Force first-person walk so the FP layer renders for the capture.
  if (!probe_forced_view_) {
    ctx_.walk_mode = true;
    ctx_.third_person = false;
    probe_forced_view_ = true;
  }
  const std::string dir = (std::string(kDir) == "1") ? std::string(".") : std::string(kDir);

  switch (probe_phase_) {
    case 0:  // let cells stream, then pick up + draw a weapon
      if (probe_t_ >= 5.0f) {
        ProbePickupNearestWeapon();
        BeginEquip();
        if (state_ == State::kSheathed) ProbeForceDraw();  // no inventory weapon: direct rig
        probe_phase_ = 1;
        probe_t_ = 0;
      }
      break;
    case 1:  // capture the draw motion (dynamic weapon action), then the held idle
      // The FP attack clip sweeps the blade out of the camera frustum for the
      // whole slash, so the draw (sword swept up into view, ~t=1.1s) is the frame
      // that reads as "weapon in motion". Capture it, then the settled idle.
      if (!swing_shot_ && probe_t_ >= 1.1f) {
        ctx_.renderer->CaptureScreenshot(dir + "/rec_fp_sword_swing.png");
        RX_INFO("FP_PROBE: captured {}/rec_fp_sword_swing.png (draw motion)", dir);
        swing_shot_ = true;
      }
      if ((state_ == State::kDrawn && probe_t_ >= 2.7f) || probe_t_ >= 9.0f) {
        ctx_.renderer->CaptureScreenshot(dir + "/rec_fp_sword_idle.png");
        RX_INFO("FP_PROBE: captured {}/rec_fp_sword_idle.png (state {})", dir,
                static_cast<int>(state_));
        probe_phase_ = 2;
        probe_t_ = 0;
      }
      break;
    case 2:  // exercise the attack clip (for the pickup->equip->swing chain log)
      if (probe_t_ >= 0.6f) {
        actors_.PlayFpClip(ActorSystem::FpClip::kAttack);
        state_ = State::kAttacking;
        RX_INFO("FP_PROBE: playing 1hm attack swing");
        probe_phase_ = 3;
        probe_t_ = 0;
      }
      break;
    case 3:  // let the swing play through, then drop
      if (probe_t_ >= 1.2f) {
        probe_phase_ = 4;
        probe_t_ = 0;
      }
      break;
    case 4:  // drop the weapon
      if (probe_t_ >= 1.5f) {
        // Drop the drawn weapon back into the world: it leaves the inventory,
        // spawns a physics-driven world item that tumbles to the floor and
        // persists to items.bin. Stay first person but aim the look down so the
        // sword lying ahead frames cleanly (no player body in the way).
        if (ctx_.items) ctx_.items->DropLast();
        state_ = State::kSheathed;
        ctx_.debug_look_pitch = -0.5f;  // provisional down-tilt while it settles
        RX_INFO("FP_PROBE: dropped weapon, aiming down for the ground shot");
        probe_phase_ = 5;
        probe_t_ = 0;
      }
      break;
    case 5:  // let the sword settle, then aim the look exactly at where it rests
      if (probe_t_ >= 3.0f) {
        Vec3 eye = ctx_.walk_eye;
        f32 best_d2 = 1e30f;
        bool found = false;
        f32 ipos[3] = {0, 0, 0};
        ctx_.world->Each<inventory::WorldItem>([&](ecs::Entity, inventory::WorldItem& wi) {
          const f32 dx = wi.last_pos[0] - eye.x, dz = wi.last_pos[2] - eye.z;
          const f32 d2 = dx * dx + dz * dz;
          if (d2 < best_d2) {
            best_d2 = d2;
            ipos[0] = wi.last_pos[0]; ipos[1] = wi.last_pos[1]; ipos[2] = wi.last_pos[2];
            found = true;
          }
        });
        if (found) {
          const f32 dx = ipos[0] - eye.x, dy = ipos[1] - eye.y, dz = ipos[2] - eye.z;
          const f32 horiz = std::sqrt(dx * dx + dz * dz);
          ctx_.debug_look_pitch = std::atan2(dy, std::max(horiz, 0.01f));
          RX_INFO("FP_PROBE: dropped sword rests at ({:.2f},{:.2f},{:.2f}) {:.1f} m ahead; pitch {:.2f}",
                  ipos[0], ipos[1], ipos[2], horiz, ctx_.debug_look_pitch);
        }
        probe_phase_ = 6;
        probe_t_ = 0;
      }
      break;
    case 6:  // let the new look pitch apply for a couple frames, then capture
      if (probe_t_ >= 0.4f) {
        ctx_.renderer->CaptureScreenshot(dir + "/rec_sword_dropped.png");
        RX_INFO("FP_PROBE: captured {}/rec_sword_dropped.png", dir);
        probe_phase_ = 7;
        probe_t_ = 0;
      }
      break;
    case 7:  // let the deferred capture land, then stop the harness
      if (probe_t_ >= 1.5f) {
        RX_INFO("FP_PROBE: done");
        probe_phase_ = 8;
      }
      break;
    default:
      break;
  }
}

}  // namespace rx
