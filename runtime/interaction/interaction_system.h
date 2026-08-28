#ifndef RECREATION_RUNTIME_INTERACTION_INTERACTION_SYSTEM_H_
#define RECREATION_RUNTIME_INTERACTION_INTERACTION_SYSTEM_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "components/bethesda/record.h"
#include "core/input_actions.h"
#include "core/types.h"
#include "core/window.h"
#include "runtime/app/engine_context.h"
#include "runtime/input/game_input.h"  // Action / Axis enums the game resolves

namespace rx {

class ActorSystem;

// Walk-mode interaction: finds the form the player faces and shows its prompt,
// opens NPC conversations and container loot views, walks load doors, and
// raises OnActivate on scripted refs. The dialogue/container session state lives
// here; the engine routes input keys to it and mirrors the HUD via SyncHud.
class InteractionSystem {
 public:
  InteractionSystem(EngineContext& ctx, ActorSystem* actors);

  void UpdateInteraction(bool activate_pressed);
  // Host-authoritative trigger volumes: a placed reference with a script and a
  // primitive bound is a trigger box; when the player enters it, its
  // OnTriggerEnter runs, and OnTriggerLeave when the player steps back out (the
  // way Skyrim drives progression from the world). Registers triggers lazily as
  // cells stream in.
  void UpdateTriggers();
  void SyncHud();  // mirror the open conversation / loot view into the HUD

  void OpenDialogue(u64 npc);
  void SelectDialogueOption(int index);
  // Headless diagnostic (RX_DIALOGUE_PROBE): opens dialogue with `npc` and logs
  // the topics it would offer, then closes it. Verifies speaker gating + topic
  // selection without the UI.
  void ReportDialogueWith(u64 npc);
  void CloseDialogue();
  void UpdateDialogueInput(const InputState& input, const ActionState& actions);

  bool TryActivateDoor(u64 handle);
  void EnterThroughDoor(bethesda::GlobalFormId dest_door, const f32 pos[3], const f32 rot[3]);
  bool TryOpenContainer(u64 handle);
  void CloseContainer();
  void UpdateContainerInput(const InputState& input, const ActionState& actions);
  // Loot the open container: move one row, or everything, into the player's
  // inventory. A taken row leaves the list and is remembered for that container
  // so reopening it does not offer the same loot again (for this session; the
  // container's own contents are read from the records each time it opens).
  void TakeContainerItem(int index);
  void TakeAllContainerItems();
  // Drop what every chest remembers giving up. The memory lives only in this
  // object, so a savegame load (which rewinds the world to a point where those
  // chests may still be full) has to clear it or the player loses loot the save
  // says is still there.
  void ResetLootMemory();

  // Authoritative entry points the server / single-player run directly (a client
  // routes the request to the server, which calls these).
  // Runs a dialogue INFO's begin fragment (the TIF_ script). owning_quest, when
  // non-zero, is registered so the fragment's GetOwningQuest() resolves.
  void RunInfoFragment(u64 info, u64 owning_quest = 0);
  void RaiseActivate(u64 handle);
  bool RaiseRemoteActivate(u32 peer, ecs::Entity player, u64 handle);
  bool AttachReferenceScripts(u64 handle);
  bethesda::GlobalFormId ReferenceForm(u64 handle) const;

  // Activation prompt state, surfaced to the quest debugger.
  u64 activate_target() const { return activate_target_; }
  const base::String& activate_label() const { return activate_label_; }
  bool dialogue_open() const { return dialogue_session_.open; }
  bool container_open() const { return container_session_.open; }

  base::String ActivationLabel(bethesda::GlobalFormId refr);
  base::String RecordName(bethesda::GlobalFormId id);

 private:
  // Moves one row into the inventory without writing the save; the public
  // entry points decide when to persist. True when anything moved.
  bool TakeContainerRow(int index);

  // One selectable conversation line plus the INFO fragment it runs.
  struct DialogueOption {
    base::String player_line;
    base::String npc_line;
    u64 info = 0;
    u64 quest = 0;
    base::String fragment_function;
  };
  struct DialogueSession {
    bool open = false;
    u64 npc = 0;
    base::String speaker;
    base::String npc_line;
    base::Vector<DialogueOption> options;
    int selected = 0;  // highlighted option for keyboard/gamepad navigation
  };
  struct ContainerItem {
    base::String name;
    i32 count = 0;
    bethesda::GlobalFormId base;  // the item record, for moving it into the inventory
    // Which CNTO subrecord this row came from. A CONT may list the same base in
    // two subrecords, so the loot memory keys on the slot rather than the base;
    // otherwise taking one stack of gold would hide the other as well.
    u32 slot = 0;
  };
  struct ContainerSession {
    bool open = false;
    u64 container = 0;
    base::String name;
    base::Vector<ContainerItem> items;
  };

  EngineContext& ctx_;
  ActorSystem* actors_;
  ecs::World& world_;
  bethesda::RecordStore& records_;
  bethesda::StringTable& strings_;
  dialogue::DialogueDb& dialogue_;
  world::QuestWorld& quest_world_;
  FlyCamera& camera_;
  GameUi& game_ui_;

  // A placed reference's trigger volume (engine space, axis-aligned box). `inside`
  // tracks whether the player is currently within it, so OnTriggerEnter fires
  // once per entry, not every frame.
  struct TriggerVolume {
    Vec3 center;
    Vec3 half_extents;
    bool inside = false;
  };

  DialogueSession dialogue_session_;
  ContainerSession container_session_;
  // Container handle -> the CNTO slots already looted out of it, so a reopened
  // chest offers only what is left. The handle is a packed REFR form id, unique
  // across cells, so one chest's memory cannot mask another's.
  base::UnorderedMap<u64, base::Vector<u32>> looted_;
  u64 activate_target_ = 0;
  base::String activate_label_;
  // Trigger references, keyed by form handle, plus the set of refs already
  // examined (so each ref's record is parsed for a primitive/script only once).
  base::UnorderedMap<u64, TriggerVolume> triggers_;
  base::UnorderedMap<u64, u8> trigger_examined_;
  struct ScriptAttachmentState {
    ecs::Entity entity = ecs::kInvalidEntity;
    bool attached = false;
    bool complete = false;
  };
  base::UnorderedMap<u64, ScriptAttachmentState> scripts_examined_;
  base::Vector<u64> trigger_scratch_;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_INTERACTION_INTERACTION_SYSTEM_H_
