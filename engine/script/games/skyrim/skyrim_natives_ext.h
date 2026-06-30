#ifndef RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_EXT_H_
#define RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_EXT_H_

#include <string>
#include <vector>

#include "core/types.h"
#include "script/games/skyrim/skyrim_natives.h"
#include "script/papyrus/native.h"
#include "script/papyrus/value.h"

// Additional native batches, one registration function per domain, each in its
// own .cc so they can be developed independently. RegisterSkyrimNatives calls
// them. The helpers below mirror the ones in skyrim_natives.cc so a batch file
// needs nothing else.
namespace rec::script::skyrim {

namespace ext {
using Args = std::vector<papyrus::Value>;

inline f32 ArgF(const Args& a, size_t i) { return i < a.size() ? a[i].ToFloat() : 0.0f; }
inline i32 ArgI(const Args& a, size_t i) { return i < a.size() ? a[i].ToInt() : 0; }
inline bool ArgB(const Args& a, size_t i, bool fallback) {
  return i < a.size() ? a[i].ToBool() : fallback;
}
inline std::string ArgS(const Args& a, size_t i) {
  return i < a.size() ? a[i].ToString() : std::string();
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

}  // namespace rec::script::skyrim

#endif  // RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVES_EXT_H_
