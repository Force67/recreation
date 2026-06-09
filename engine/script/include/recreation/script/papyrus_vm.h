#ifndef RECREATION_SCRIPT_PAPYRUS_VM_H_
#define RECREATION_SCRIPT_PAPYRUS_VM_H_

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "recreation/bethesda/game_profile.h"
#include "recreation/core/types.h"
#include "recreation/ecs/world.h"

namespace rec::script {

// Compiled papyrus script (.pex). The bytecode format is shared across the
// games, the native function surface is not: Skyrim's Game.GetPlayer() does
// not exist in Fallout, FO4 adds whole namespaces. Each game gets its own
// native function table bound against engine systems, which is the one place
// in the engine that stays per game.
struct PexScript {
  std::string name;
  std::vector<u8> bytecode;
};

using NativeFn = std::function<void(ecs::World& world, std::vector<u64>& stack)>;

class PapyrusVm {
 public:
  explicit PapyrusVm(bethesda::Game game);

  bool LoadScript(ByteSpan pex_data);

  // Binds the native function tables for the configured game. Unbound
  // natives log once and return defaults, so partially supported mods keep
  // running instead of hard failing.
  void BindNatives();

  void Tick(ecs::World& world, f32 dt);

 private:
  bethesda::Game game_;
  std::unordered_map<std::string, PexScript> scripts_;
  std::unordered_map<std::string, NativeFn> natives_;
};

}  // namespace rec::script

#endif  // RECREATION_SCRIPT_PAPYRUS_VM_H_
