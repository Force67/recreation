#ifndef RECREATION_SCRIPT_SCRIPT_SYSTEM_H_
#define RECREATION_SCRIPT_SCRIPT_SYSTEM_H_

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "asset/vfs.h"
#include "bethesda/game_profile.h"
#include "bethesda/script_attachment.h"
#include "core/types.h"
#include "script/games/skyrim/skyrim_natives.h"
#include "script/papyrus/value.h"
#include "script/papyrus_guest.h"

namespace rec::script {

// Ties the Papyrus guest to game data. It loads compiled scripts (and their
// ancestor chain) on demand from the asset VFS, instantiates the scripts a form
// carries (from its VMAD), seeds their properties from the editor-baked values,
// raises OnInit, and ticks the guest. Instances are keyed by form id, so an
// object property pointing at another form resolves to that form's instance.
//
// This is the engine's entry into Papyrus: the runtime owns one ScriptSystem,
// feeds it the forms it streams in, and ticks it each frame.
class ScriptSystem {
 public:
  // bindings supplies the engine side of the Skyrim native surface (may be
  // null for neutral defaults). vfs provides scripts/<name>.pex.
  ScriptSystem(bethesda::Game game, asset::Vfs* vfs, skyrim::SkyrimBindings* bindings);
  ~ScriptSystem();

  // Loads name.pex and its ancestor chain from the VFS into the guest. Returns
  // the loaded type name, or "" if the root script is missing. Idempotent.
  std::string EnsureScriptLoaded(const std::string& name);

  // Instantiates every script attached to form_id, seeds properties, raises
  // OnInit, and returns the created instance handles. Re-attaching to a form
  // that already has instances is a no-op for the already-present scripts.
  std::vector<papyrus::ObjectRef> AttachScripts(u64 form_id, const bethesda::ScriptAttachment& att);

  // Advances guest time, firing any due update events.
  void Tick(f32 dt);

  // Invoked (main thread) with a form id each time that form's scripts attach,
  // i.e. the form goes live in the world. The runtime uses it to notify the
  // managed scripting world (a FormLoaded event). Only fires when at least one
  // script instance was created.
  void set_on_scripts_attached(std::function<void(u64)> cb) { on_attach_ = std::move(cb); }

  PapyrusGuest& guest() { return guest_; }
  size_t loaded_script_count();

 private:
  asset::Vfs* vfs_;
  PapyrusGuest guest_;
  std::function<void(u64)> on_attach_;
  // Script names we have already reported as unloadable, so a game whose
  // bytecode the VM cannot execute yet (Starfield) warns once per script
  // instead of once per attachment.
  std::unordered_set<std::string> warned_unloadable_;
};

}  // namespace rec::script

#endif  // RECREATION_SCRIPT_SCRIPT_SYSTEM_H_
