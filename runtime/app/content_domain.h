#ifndef RECREATION_RUNTIME_APP_CONTENT_DOMAIN_H_
#define RECREATION_RUNTIME_APP_CONTENT_DOMAIN_H_

#include <base/memory/unique_pointer.h>
#include <base/strings/xstring.h>

#include "asset/asset_database.h"
#include "asset/vfs.h"
#include "components/bethesda/game_profile.h"
#include "components/bethesda/load_order.h"
#include "components/bethesda/record.h"
#include "components/bethesda/strings.h"
#include "components/dialogue/dialogue.h"
#include "components/script/games/skyrim/skyrim_bindings.h"
#include "components/script/script_system.h"
#include "core/types.h"

namespace rx {

// One game's content fully enclosed: its archives and loose files (vfs), the
// converted asset cache, the merged record store, localized strings, dialogue
// index, the native bindings and a Papyrus guest (its own isolated microvm).
//
// recreation loads one domain per game and runs them side by side, so Skyrim
// and Fallout 4 content can be live in the same process at the same time. Form
// ids and asset paths collide across games, so each domain keeps its own vfs
// and record store; the renderer, ECS and physics are shared by the runtime.
//
// A domain is self contained: Load brings up the data and the guest VM and
// instantiates the game's quest scripts, so its microvm runs that game's
// scripted logic on its own. The runtime layers the engine wiring (world
// command sink, notifications, the managed C# bridge) on top.
class ContentDomain {
 public:
  ContentDomain() = default;

  // Detects (or forces) the game in data_dir, mounts its archives, loads the
  // record store from plugins_txt, builds strings/dialogue, and brings up the
  // bindings + guest VM. replica_mode marks a multiplayer client whose scripts
  // must not mutate authoritative state. Returns false if the data is missing
  // or no supported game is present.
  bool Load(bethesda::Game game,
            const base::String& data_dir,
            const base::String& plugins_txt,
            bool replica_mode);

  // Instantiates every quest with a Papyrus script so the domain's microvm runs
  // that game's quests, and registers each quest definition + stage fragments on
  // the guest. Returns the number of quests instantiated. Self contained: unlike
  // the runtime's QuestDirector it does not build HUD/compass state, so a non
  // rendered (secondary) domain still runs its quest logic.
  int AttachQuestScripts(int max_quests);

  // Advances the domain's guest VM.
  void Tick(f32 dt);

  bethesda::Game game() const { return game_; }
  const bethesda::GameProfile& profile() const { return *profile_; }
  const base::String& data_dir() const { return data_dir_; }

  asset::Vfs& vfs() { return vfs_; }
  asset::AssetDatabase& assets() { return *assets_; }
  bethesda::RecordStore& records() { return records_; }
  bethesda::StringTable& strings() { return strings_; }
  dialogue::DialogueDb& dialogue() { return dialogue_; }
  // base::UniquePointer has no get(); the address of the pointee is the
  // non-owning handle callers want.
  script::skyrim::RecordBackedSkyrimBindings* bindings() {
    return bindings_ ? &*bindings_ : nullptr;
  }
  script::ScriptSystem* scripts() { return scripts_ ? &*scripts_ : nullptr; }

 private:
  bethesda::Game game_ = bethesda::Game::kUnknown;
  const bethesda::GameProfile* profile_ = nullptr;
  base::String data_dir_;

  asset::Vfs vfs_;
  base::UniquePointer<asset::AssetDatabase> assets_;
  bethesda::RecordStore records_;
  bethesda::StringTable strings_;
  dialogue::DialogueDb dialogue_;
  // bindings_ before scripts_: the guest thread (which calls the bindings) is
  // joined in ScriptSystem's destructor before the bindings are torn down.
  base::UniquePointer<script::skyrim::RecordBackedSkyrimBindings> bindings_;
  base::UniquePointer<script::ScriptSystem> scripts_;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_APP_CONTENT_DOMAIN_H_
