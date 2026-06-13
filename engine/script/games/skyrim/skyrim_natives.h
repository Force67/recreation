#ifndef RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_H_
#define RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_H_

#include <string>

#include "core/types.h"
#include "script/papyrus/native.h"
#include "script/papyrus/value.h"

namespace rec::script::skyrim {

// Everything the Skyrim native surface needs from the engine. The engine
// implements the slices it supports; the rest keep the defaults here, so a
// partially wired engine still runs scripts (they just see neutral values).
// The guest never sees an engine type, only this interface, which keeps the two
// worlds decoupled. A Fallout binding would be a separate interface paired with
// a separate native table.
class SkyrimBindings {
 public:
  virtual ~SkyrimBindings() = default;

  // Game
  virtual papyrus::ObjectRef GetPlayer() { return {}; }

  // Form
  virtual u32 GetFormId(papyrus::ObjectRef form) { return 0; }

  // ObjectReference spatial state (engine transform / ECS).
  virtual f32 GetPositionX(papyrus::ObjectRef ref) { return 0; }
  virtual f32 GetPositionY(papyrus::ObjectRef ref) { return 0; }
  virtual f32 GetPositionZ(papyrus::ObjectRef ref) { return 0; }
  virtual void SetPosition(papyrus::ObjectRef ref, f32 x, f32 y, f32 z) {}
  virtual f32 GetDistance(papyrus::ObjectRef a, papyrus::ObjectRef b) { return 0; }
  virtual void MoveTo(papyrus::ObjectRef ref, papyrus::ObjectRef target) {}
  virtual void SetEnabled(papyrus::ObjectRef ref, bool enabled) {}

  // Actor values and state.
  virtual f32 GetActorValue(papyrus::ObjectRef actor, const std::string& av) { return 0; }
  virtual void SetActorValue(papyrus::ObjectRef actor, const std::string& av, f32 value) {}
  virtual void ModActorValue(papyrus::ObjectRef actor, const std::string& av, f32 delta) {}
  virtual i32 GetLevel(papyrus::ObjectRef actor) { return 1; }
  virtual bool IsDead(papyrus::ObjectRef actor) { return false; }

  // Time (real seconds since start; in-game clock handled by the engine).
  virtual f32 GetRealHoursPassed() { return 0; }
};

// Registers the Skyrim Papyrus native surface into reg, bound against bindings
// (pass nullptr to use neutral defaults). Math/Utility globals are fully
// implemented here; engine-touching natives route through bindings.
void RegisterSkyrimNatives(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);

}  // namespace rec::script::skyrim

#endif  // RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_H_
