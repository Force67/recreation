#include "interaction_system.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "actor_system.h"
#include "bethesda/script_attachment.h"
#include "core/log.h"
#include "core/math.h"
#include "script/games/skyrim/skyrim_condition_context.h"
#include "script/papyrus/value.h"
#include "world/components.h"
#include "world/interaction.h"

namespace rec {

InteractionSystem::InteractionSystem(EngineContext& ctx, ActorSystem* actors)
    : ctx_(ctx),
      actors_(actors),
      world_(*ctx.world),
      records_(*ctx.records),
      strings_(*ctx.strings),
      dialogue_(*ctx.dialogue),
      quest_world_(*ctx.quest_world),
      camera_(*ctx.camera),
      game_ui_(*ctx.game_ui) {}

void InteractionSystem::SyncHud() {
  // Mirror the conversation state into the HUD.
  DialogueView dv;
  dv.open = dialogue_session_.open;
  dv.speaker = dialogue_session_.speaker;
  dv.npc_line = dialogue_session_.npc_line;
  for (const DialogueOption& o : dialogue_session_.options) dv.options.push_back(o.player_line);
  game_ui_.SetDialogue(dv);

  // Mirror the open container's contents into the HUD loot panel.
  ContainerView cv;
  cv.open = container_session_.open;
  cv.name = container_session_.name;
  for (const ContainerItem& it : container_session_.items) cv.items.push_back({it.name, it.count});
  game_ui_.SetContainer(cv);
}

std::string InteractionSystem::RecordName(bethesda::GlobalFormId id) {
  bethesda::Record record;
  if (!records_.Parse(id, &record)) return {};
  const bethesda::Subrecord* full = record.Find(FourCc('F', 'U', 'L', 'L'));
  if (!full) return {};
  // A localized plugin stores a 4-byte string id here; a non-localized one
  // stores the literal text. Try the table first, fall back to the bytes.
  if (full->data.size() >= 4) {
    u32 string_id;
    std::memcpy(&string_id, full->data.data(), 4);
    if (const base::String* s = strings_.Find(string_id)) return std::string(s->c_str());
  }
  return record.GetString(FourCc('F', 'U', 'L', 'L'));
}

std::string InteractionSystem::ActivationLabel(bethesda::GlobalFormId refr) {
  const bethesda::RecordStore::StoredRecord* stored = records_.Find(refr);
  if (!stored) return "Activate";
  bethesda::Record record;
  if (!records_.Parse(refr, &record)) return "Activate";

  // The placed reference points at its base object through NAME; the base
  // carries the displayed name and the type that picks the verb.
  std::string verb = "Activate";
  std::string name;
  if (const bethesda::Subrecord* nm = record.Find(FourCc('N', 'A', 'M', 'E')); nm && nm->data.size() >= 4) {
    u32 raw;
    std::memcpy(&raw, nm->data.data(), 4);
    bethesda::GlobalFormId base = records_.ResolveFrom(bethesda::RawFormId{raw}, stored->winning_plugin);
    if (const bethesda::RecordStore::StoredRecord* bstored = records_.Find(base)) {
      switch (bstored->header.type) {
        case FourCc('N', 'P', 'C', '_'):
        case FourCc('A', 'C', 'H', 'R'): verb = "Talk to"; break;
        case FourCc('D', 'O', 'O', 'R'):
        case FourCc('C', 'O', 'N', 'T'): verb = "Open"; break;
        default: break;
      }
      name = RecordName(base);
    }
  }
  return name.empty() ? verb : verb + " " + name;
}

void InteractionSystem::UpdateInteraction(bool activate_pressed) {
  // Activation is a walk-mode affordance and needs the guest to dispatch events.
  if (!ctx_.walk_mode || !actors_->HasPlayer() || !ctx_.scripts || !ctx_.bindings) {
    if (activate_target_ != 0) {
      activate_target_ = 0;
      game_ui_.SetActivatePrompt("");
    }
    return;
  }

  // Skyrim's activation reach is ~150 units; the world is meters (~70 units/m).
  constexpr f32 kRange = 2.2f;
  constexpr f32 kFacingDot = 0.45f;  // ~63 degree cone
  // Reach from the player's head, not the camera: in third person the eye sits
  // metres behind the body and would put everything in front out of range.
  Vec3 eye = ctx_.walk_eye;
  Vec3 ppos;
  if (actors_->PlayerWorldPos(&ppos)) eye = Vec3{ppos.x, ppos.y + 1.6f, ppos.z};
  const Vec3 fwd = Normalize(ctx_.walk_target - ctx_.walk_eye);

  // Collect the form-linked refs near the eye; a coarse cull keeps this cheap
  // even in a dense cell, and the cap bounds a pathological frame.
  base::Vector<world::ActivationCandidate> candidates;
  const f32 coarse_sq = (kRange + 0.5f) * (kRange + 0.5f);
  world_.Each<world::FormLink, world::Transform>(
      [&](ecs::Entity, world::FormLink& link, world::Transform& t) {
        if (candidates.size() >= 512) return;
        const f32 dx = t.position[0] - eye.x, dy = t.position[1] - eye.y, dz = t.position[2] - eye.z;
        if (dx * dx + dy * dy + dz * dz > coarse_sq) return;
        world::ActivationCandidate c;
        c.form_handle = link.form.packed();
        c.pos[0] = t.position[0];
        c.pos[1] = t.position[1];
        c.pos[2] = t.position[2];
        candidates.push_back(c);
      });

  const float p[3] = {eye.x, eye.y, eye.z};
  const float f[3] = {fwd.x, fwd.y, fwd.z};
  const int idx = world::PickActivationTarget(p, f, candidates.data(),
                                              static_cast<int>(candidates.size()), kRange, kFacingDot);
  if (idx < 0) {
    if (activate_target_ != 0) {
      activate_target_ = 0;
      game_ui_.SetActivatePrompt("");
    }
    return;
  }

  const u64 handle = candidates[idx].form_handle;
  if (handle != activate_target_) {
    activate_target_ = handle;
    bethesda::GlobalFormId form{static_cast<u16>(handle >> 32),
                                static_cast<u32>(handle & 0xffffffffu)};
    activate_label_ = ActivationLabel(form);
    game_ui_.SetActivatePrompt(activate_label_ + "   [E]");
  }

  if (activate_pressed) {
    REC_INFO("activate: {} (0x{:x})", activate_label_, handle);
    // Raise the player-use hook for mods (gmod-style) before the built-in
    // affordances. Fires on the main thread; drained into managed next frame.
    if (ctx_.managed)
      ctx_.managed->QueueEvent({script::host::ManagedEventId::kPlayerActivated, handle, 0, 0, 0.0f});
    // Local-view affordances first: talking to an NPC, walking through a load
    // door, and opening a container are each the activating player's own view.
    // Anything else raises OnActivate, which a multiplayer client is not
    // authoritative for -- it asks the server.
    const ecs::Entity e = quest_world_.Find(handle);
    if (world_.IsAlive(e) && world_.Has<world::Npc>(e)) {
      OpenDialogue(handle);
    } else if (TryActivateDoor(handle)) {
      // Entered a load door (teleport / cell transition).
    } else if (TryOpenContainer(handle)) {
      // Opened a container's loot view.
#if RECREATION_HAS_NET
    } else if (ctx_.client_session && ctx_.client_session->joined()) {
      ctx_.client_session->SendActivate(handle);
#endif
    } else {
      RaiseActivate(handle);
    }
  }
}

void InteractionSystem::OpenDialogue(u64 npc) {
  if (!ctx_.scripts || !ctx_.bindings) return;
  auto* binds = ctx_.bindings;
  // Gather the NPC's available topics on the guest thread, which owns the quest
  // state the conditions read; records_/dialogue_/strings_ are read-only here.
  dialogue_session_ =
      ctx_.scripts->guest()
          .SubmitFor([this, npc, binds](script::papyrus::VirtualMachine&) {
            DialogueSession s;
            s.npc = npc;
            s.open = true;
            s.speaker = binds->GetName(script::papyrus::ObjectRef{npc});
            // The actor's base form, to gate lines keyed to a speaker by GetIsID.
            const u64 speaker_base = binds->GetBaseObject(script::papyrus::ObjectRef{npc}).handle;
            // A response is offered to this NPC when it carries no GetIsID
            // speaker condition (a line anyone may say) or one that names this
            // actor's base; otherwise it belongs to a different speaker.
            auto for_speaker = [&](const dialogue::Response& r) {
              bool keyed = false;
              for (const quest::Comparison& c : r.conditions.comparisons) {
                if (c.func != quest::Func::kGetIsId) continue;
                keyed = true;
                if (c.param1 == speaker_base) return true;
              }
              return !keyed;
            };
            script::skyrim::SkyrimConditionContext ctx(binds);
            for (const quest::QuestStatus& q : binds->quest_system().AllStatuses()) {
              if (!q.running) continue;
              for (dialogue::Handle dial : dialogue_.TopicsForQuest(q.handle)) {
                bethesda::GlobalFormId id{static_cast<u16>(dial >> 32),
                                          static_cast<u32>(dial & 0xffffffffu)};
                dialogue::Topic t = dialogue::ParseTopic(records_, id, &strings_);
                if (t.dial == 0) continue;
                for (const dialogue::Response& r : t.responses) {
                  if (s.options.size() >= 4) break;
                  // Gate by speaker, then by the conditions we can judge
                  // (Allows shows the line unless an understood check fails).
                  if (!for_speaker(r) || !ctx.Allows(r.conditions)) continue;
                  DialogueOption opt;
                  opt.player_line = r.player_line;
                  opt.npc_line = r.npc_line;
                  opt.info = r.info;
                  opt.quest = q.handle;
                  opt.fragment_function = r.fragment_function;
                  s.options.push_back(std::move(opt));
                }
                if (s.options.size() >= 4) break;
              }
              if (s.options.size() >= 4) break;
            }
            return s;
          })
          .get();
  REC_INFO("dialogue: opened with '{}' ({} topics)", dialogue_session_.speaker,
           dialogue_session_.options.size());
}

void InteractionSystem::SelectDialogueOption(int index) {
  if (!dialogue_session_.open || index < 0 ||
      index >= static_cast<int>(dialogue_session_.options.size()))
    return;
  const DialogueOption opt = dialogue_session_.options[index];
  dialogue_session_.npc_line = opt.npc_line;  // show the reply
  // Firing the INFO fragment (which advances the quest) is server-authoritative:
  // a client asks the server, the host / single-player runs it directly.
#if RECREATION_HAS_NET
  if (ctx_.client_session && ctx_.client_session->joined())
    ctx_.client_session->SendDialogueSelect(opt.info);
  else
#endif
    RunInfoFragment(opt.info);
}

void InteractionSystem::RunInfoFragment(u64 info) {
  if (!ctx_.scripts || info == 0) return;
  const bethesda::GlobalFormId id{static_cast<u16>(info >> 32),
                                  static_cast<u32>(info & 0xffffffffu)};
  bethesda::Record record;
  if (!records_.Parse(id, &record)) return;
  const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D'));
  if (!vmad) return;
  bethesda::ScriptAttachment attachment;
  bethesda::InfoFragments frags;
  if (!bethesda::ParseInfoFragments(vmad->data, &attachment, &frags)) return;
  if (frags.begin.function.empty()) return;
  // Attach the TIF_ script to the INFO handle (idempotent -- only creates the
  // instance + seeds properties the first time), then run the begin fragment on
  // it. Its seeded quest property lets the fragment SetStage the right quest.
  ctx_.scripts->AttachScripts(info, attachment);
  const std::string fn = frags.begin.function;
  ctx_.scripts->guest().Submit([info, fn](script::papyrus::VirtualMachine& vm) {
    vm.TryCall(script::papyrus::ObjectRef{info}, fn, {});
  });
}

void InteractionSystem::CloseDialogue() { dialogue_session_ = DialogueSession{}; }

void InteractionSystem::UpdateDialogueInput(const InputState& input) {
  if (!dialogue_session_.open) return;
  if (input.key_pressed(Key::kEscape)) {
    CloseDialogue();
    return;
  }
  if (input.key_pressed(Key::k1)) SelectDialogueOption(0);
  else if (input.key_pressed(Key::k2)) SelectDialogueOption(1);
  else if (input.key_pressed(Key::k3)) SelectDialogueOption(2);
  else if (input.key_pressed(Key::k4)) SelectDialogueOption(3);
}

void InteractionSystem::RaiseActivate(u64 handle) {
  if (!ctx_.scripts) return;
  // OnActivate(player). Scripted activators and NPCs run their authored response,
  // which can set quest stages. The activator is the single Skyrim player ref;
  // multiplayer treats any client's activation as "the player" for quest logic.
  ctx_.scripts->guest().RaiseEvent(
      script::papyrus::ObjectRef{handle}, "OnActivate",
      {script::papyrus::Value::Object(script::papyrus::ObjectRef{0x14})});
}

void InteractionSystem::UpdateTriggers() {
  // Host-authoritative, walk-mode only; a client receives the resulting quest
  // changes through replication rather than firing triggers itself.
  if (!ctx_.walk_mode || !actors_->HasPlayer() || !ctx_.scripts) return;
#if RECREATION_HAS_NET
  if (ctx_.client_session) return;
#endif

  // Examine each placed reference once: one carrying both a script (VMAD) and a
  // primitive bound (XPRM) is a trigger box. Attach its script so OnTriggerEnter
  // can dispatch, and register its world-space volume.
  world_.Each<world::FormLink, world::Transform>(
      [&](ecs::Entity, world::FormLink& link, world::Transform& t) {
        const u64 handle = link.form.packed();
        if (trigger_examined_.find(handle)) return;
        trigger_examined_.insert(handle, 1);
        const bethesda::GlobalFormId id{static_cast<u16>(handle >> 32),
                                        static_cast<u32>(handle & 0xffffffffu)};
        bethesda::Record record;
        if (!records_.Parse(id, &record)) return;
        const bethesda::Subrecord* xprm = record.Find(FourCc('X', 'P', 'R', 'M'));
        const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D'));
        if (!xprm || xprm->data.size() < 12 || !vmad) return;  // not a scripted trigger box
        bethesda::ScriptAttachment att;
        if (!bethesda::ParseScriptAttachment(vmad->data, &att) || att.scripts.empty()) return;
        ctx_.scripts->AttachScripts(handle, att);
        // XPRM opens with the box half-extents (Bethesda units); map to engine
        // axes (x, z, -y) and metres, as a reference's world position is.
        f32 b[3];
        std::memcpy(b, xprm->data.data(), 12);
        constexpr f32 s = 0.01428f;
        TriggerVolume vol;
        vol.center = Vec3{t.position[0], t.position[1], t.position[2]};
        vol.half_extents = Vec3{std::fabs(b[0]) * s, std::fabs(b[2]) * s, std::fabs(b[1]) * s};
        triggers_.insert(handle, vol);
        REC_INFO("trigger: registered scripted volume 0x{:x}", handle);
      });

  Vec3 player;
  if (!actors_->PlayerWorldPos(&player)) return;

  // Fire OnTriggerEnter once as the player crosses into a volume; drop volumes
  // whose reference streamed out (and re-examine it if it streams back in).
  trigger_scratch_.clear();
  for (auto entry : triggers_) {
    const ecs::Entity e = quest_world_.Find(entry.key);
    if (!world_.IsAlive(e)) {
      trigger_scratch_.push_back(entry.key);
      continue;
    }
    if (const world::Transform* t = world_.Get<world::Transform>(e))
      entry.value.center = Vec3{t->position[0], t->position[1], t->position[2]};
    const TriggerVolume& v = entry.value;
    const bool inside = std::fabs(player.x - v.center.x) <= v.half_extents.x &&
                        std::fabs(player.y - v.center.y) <= v.half_extents.y &&
                        std::fabs(player.z - v.center.z) <= v.half_extents.z;
    if (inside && !entry.value.inside) {
      REC_INFO("trigger: player entered 0x{:x}, raising OnTriggerEnter", entry.key);
      ctx_.scripts->guest().RaiseEvent(
          script::papyrus::ObjectRef{entry.key}, "OnTriggerEnter",
          {script::papyrus::Value::Object(script::papyrus::ObjectRef{0x14})});
    }
    entry.value.inside = inside;
  }
  for (u64 key : trigger_scratch_) {
    triggers_.erase(key);
    trigger_examined_.erase(key);
  }
}

bool InteractionSystem::TryActivateDoor(u64 handle) {
  if (!ctx_.streamer) return false;
  bethesda::GlobalFormId refr{static_cast<u16>(handle >> 32),
                              static_cast<u32>(handle & 0xffffffffu)};
  const bethesda::RecordStore::StoredRecord* stored = records_.Find(refr);
  if (!stored) return false;
  bethesda::Record record;
  if (!records_.Parse(refr, &record)) return false;

  // The reference must be a DOOR (its base object's type).
  const bethesda::Subrecord* nm = record.Find(FourCc('N', 'A', 'M', 'E'));
  if (!nm || nm->data.size() < 4) return false;
  u32 base_raw;
  std::memcpy(&base_raw, nm->data.data(), 4);
  bethesda::GlobalFormId base = records_.ResolveFrom(bethesda::RawFormId{base_raw},
                                                     stored->winning_plugin);
  const bethesda::RecordStore::StoredRecord* bstored = records_.Find(base);
  if (!bstored || bstored->header.type != FourCc('D', 'O', 'O', 'R')) return false;

  // XTEL on the reference is the teleport: dest door form id (4), then the
  // landing position (3 floats) and rotation (3 floats). A door without one is
  // not a load door -- let it fall through to its OnActivate script.
  const bethesda::Subrecord* xtel = record.Find(FourCc('X', 'T', 'E', 'L'));
  if (!xtel || xtel->data.size() < 28) return false;
  u32 dest_raw;
  f32 pos[3], rot[3];
  std::memcpy(&dest_raw, xtel->data.data(), 4);
  std::memcpy(pos, xtel->data.data() + 4, 12);
  std::memcpy(rot, xtel->data.data() + 16, 12);
  bethesda::GlobalFormId dest = records_.ResolveFrom(bethesda::RawFormId{dest_raw},
                                                     stored->winning_plugin);
  EnterThroughDoor(dest, pos, rot);
  return true;
}

void InteractionSystem::EnterThroughDoor(bethesda::GlobalFormId dest_door, const f32 pos[3], const f32 rot[3]) {
  // The destination door's parent interior cell (if any) decides the
  // transition: stream that interior, or resume the exterior worldspace.
  bethesda::GlobalFormId interior = records_.InteriorCellOfRef(dest_door);
  if (interior.plugin != 0xffff) {
    Vec3 spawn;
    if (!ctx_.streamer->EnterInterior(world_, interior, &spawn)) {
      REC_WARN("door: failed to enter interior {:04x}:{:06x}", interior.plugin, interior.local_id);
      return;
    }
    REC_INFO("door: entered interior {:04x}:{:06x}", interior.plugin, interior.local_id);
  } else {
    ctx_.streamer->EnterExterior(world_);
    REC_INFO("door: returned to the exterior worldspace");
  }

  // Bethesda -> engine (mirrors CellStreamer): meters, axes (x, z, -y).
  constexpr f32 kToMeters = 0.01428f;
  const Vec3 landing{pos[0] * kToMeters, pos[2] * kToMeters, -pos[1] * kToMeters};
  actors_->TeleportPlayer(landing.x, landing.y, landing.z);
  // Face the way the door points (rot[2] is the yaw about the Bethesda up axis,
  // which maps directly to the walk camera yaw) and re-anchor streaming at the
  // landing so the destination cells load next tick.
  ctx_.cam_yaw = rot[2];
  camera_.set_position(Vec3{landing.x, landing.y + 1.6f, landing.z});
}

bool InteractionSystem::TryOpenContainer(u64 handle) {
  bethesda::GlobalFormId refr{static_cast<u16>(handle >> 32),
                              static_cast<u32>(handle & 0xffffffffu)};
  const bethesda::RecordStore::StoredRecord* stored = records_.Find(refr);
  if (!stored) return false;
  bethesda::Record record;
  if (!records_.Parse(refr, &record)) return false;
  const bethesda::Subrecord* nm = record.Find(FourCc('N', 'A', 'M', 'E'));
  if (!nm || nm->data.size() < 4) return false;
  u32 base_raw;
  std::memcpy(&base_raw, nm->data.data(), 4);
  bethesda::GlobalFormId base = records_.ResolveFrom(bethesda::RawFormId{base_raw},
                                                     stored->winning_plugin);
  const bethesda::RecordStore::StoredRecord* bstored = records_.Find(base);
  if (!bstored || bstored->header.type != FourCc('C', 'O', 'N', 'T')) return false;
  bethesda::Record cont;
  if (!records_.Parse(base, &cont)) return false;

  ContainerSession s;
  s.open = true;
  s.container = handle;
  s.name = RecordName(base);
  if (s.name.empty()) s.name = "Container";
  // CNTO holds the contents: item form id (4) + count (4). Names resolve against
  // the base record's owning plugin; the row pool caps how many we show.
  for (const bethesda::Subrecord& sub : cont.subrecords) {
    if (s.items.size() >= 14) break;
    if (sub.type != FourCc('C', 'N', 'T', 'O') || sub.data.size() < 8) continue;
    u32 item_raw;
    i32 count;
    std::memcpy(&item_raw, sub.data.data(), 4);
    std::memcpy(&count, sub.data.data() + 4, 4);
    bethesda::GlobalFormId item = records_.ResolveFrom(bethesda::RawFormId{item_raw},
                                                       bstored->winning_plugin);
    ContainerItem ci;
    ci.count = count;
    ci.name = RecordName(item);
    if (ci.name.empty()) ci.name = "(item)";
    s.items.push_back(std::move(ci));
  }
  container_session_ = std::move(s);
  REC_INFO("container: opened '{}' ({} items)", container_session_.name,
           container_session_.items.size());
  return true;
}

void InteractionSystem::CloseContainer() { container_session_ = ContainerSession{}; }

void InteractionSystem::UpdateContainerInput(const InputState& input) {
  if (!container_session_.open) return;
  if (input.key_pressed(Key::kEscape)) CloseContainer();
}

}  // namespace rec
