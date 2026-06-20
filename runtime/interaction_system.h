#ifndef RECREATION_RUNTIME_INTERACTION_SYSTEM_H_
#define RECREATION_RUNTIME_INTERACTION_SYSTEM_H_

#include <string>
#include <vector>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "bethesda/record.h"
#include "core/types.h"
#include "core/window.h"
#include "engine_context.h"

namespace rec {

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
  // OnTriggerEnter runs (which can advance a quest, the way Skyrim drives
  // progression from the world). Registers triggers lazily as cells stream in.
  void UpdateTriggers();
  void SyncHud();  // mirror the open conversation / loot view into the HUD

  void OpenDialogue(u64 npc);
  void SelectDialogueOption(int index);
  void CloseDialogue();
  void UpdateDialogueInput(const InputState& input);

  bool TryActivateDoor(u64 handle);
  void EnterThroughDoor(bethesda::GlobalFormId dest_door, const f32 pos[3], const f32 rot[3]);
  bool TryOpenContainer(u64 handle);
  void CloseContainer();
  void UpdateContainerInput(const InputState& input);

  // Authoritative entry points the server / single-player run directly (a client
  // routes the request to the server, which calls these).
  void RunInfoFragment(u64 info);
  void RaiseActivate(u64 handle);

  // Activation prompt state, surfaced to the quest debugger.
  u64 activate_target() const { return activate_target_; }
  const std::string& activate_label() const { return activate_label_; }
  bool dialogue_open() const { return dialogue_session_.open; }
  bool container_open() const { return container_session_.open; }

  std::string ActivationLabel(bethesda::GlobalFormId refr);
  std::string RecordName(bethesda::GlobalFormId id);

 private:
  // One selectable conversation line plus the INFO fragment it runs.
  struct DialogueOption {
    std::string player_line;
    std::string npc_line;
    u64 info = 0;
    u64 quest = 0;
    std::string fragment_function;
  };
  struct DialogueSession {
    bool open = false;
    u64 npc = 0;
    std::string speaker;
    std::string npc_line;
    std::vector<DialogueOption> options;
  };
  struct ContainerItem {
    std::string name;
    i32 count = 0;
  };
  struct ContainerSession {
    bool open = false;
    u64 container = 0;
    std::string name;
    std::vector<ContainerItem> items;
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
  u64 activate_target_ = 0;
  std::string activate_label_;
  // Trigger references, keyed by form handle, plus the set of refs already
  // examined (so each ref's record is parsed for a primitive/script only once).
  base::UnorderedMap<u64, TriggerVolume> triggers_;
  base::UnorderedMap<u64, u8> trigger_examined_;
  base::Vector<u64> trigger_scratch_;
};

}  // namespace rec

#endif  // RECREATION_RUNTIME_INTERACTION_SYSTEM_H_
