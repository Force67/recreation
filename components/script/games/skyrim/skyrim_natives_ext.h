#ifndef RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_EXT_H_
#define RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_EXT_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "components/script/games/skyrim/skyrim_natives.h"
#include "components/script/papyrus/native.h"
#include "components/script/papyrus/value.h"
#include "core/types.h"

// Additional native batches, one registration function per domain, each in its
// own .cc so they can be developed independently. RegisterSkyrimNatives calls
// them. The helpers below mirror the ones in skyrim_natives.cc so a batch file
// needs nothing else.
namespace rx::script::skyrim {

namespace ext {
using Args = base::Vector<papyrus::Value>;

inline f32 ArgF(const Args& a, size_t i) {
  return i < a.size() ? a[i].ToFloat() : 0.0f;
}
inline i32 ArgI(const Args& a, size_t i) {
  return i < a.size() ? a[i].ToInt() : 0;
}
inline bool ArgB(const Args& a, size_t i, bool fallback) {
  return i < a.size() ? a[i].ToBool() : fallback;
}
inline base::String ArgS(const Args& a, size_t i) {
  return i < a.size() ? a[i].ToString() : base::String();
}
inline papyrus::ObjectRef ArgO(const Args& a, size_t i) {
  return i < a.size() ? a[i].as_object() : papyrus::ObjectRef{};
}
inline SkyrimBindings& Resolve(SkyrimBindings* bindings) {
  static SkyrimBindings kDefault;
  return bindings ? *bindings : kDefault;
}
}  // namespace ext

// Game environment and settings derived from the game clock and GMST table.
void RegisterGameEnvironment(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);

// Utility helpers that compute from the game clock or report fixed engine state.
void RegisterUtilityExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);

// Actor and ObjectReference getters composed from the existing binding surface.
void RegisterActorRefGetters(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);

// One registration function per type, each in its own .cc. They cover the rest
// of the declared native surface: stateful set/get pairs round-trip through the
// shared state store, getters route to bindings or compute, and pure engine
// commands are wired no-ops until their subsystem exists.
void RegisterActorExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterObjectRefExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterGameExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterDebugExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterActiveMagicEffectExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterFactionExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterAliasExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterFormExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterWorldExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterAudioVisualExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterItemsExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterUtilityRest(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);
void RegisterVehicleExtra(papyrus::NativeRegistry& reg, SkyrimBindings* bindings);

}  // namespace rx::script::skyrim

#endif  // RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_EXT_H_
