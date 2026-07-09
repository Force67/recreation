#ifndef RECREATION_QUEST_SCENE_COMPILE_H_
#define RECREATION_QUEST_SCENE_COMPILE_H_

#include <functional>

#include "core/types.h"
#include "quest/scene.h"
#include "quest/scene_record.h"

namespace rx::quest {

// Resolvers the runtime supplies to turn a parsed SceneDef (form ids + alias
// indices) into a runnable Scene (live handles + world positions). Kept as
// callbacks so the compiler is engine-agnostic and unit-testable with mocks: the
// engine backs `actor` with the quest alias table, `info` with the dialogue db
// filtered to the speaker, and `package_target` with the PACK travel target's
// world position.
struct SceneBindings {
  // Scene actor alias index -> performer form handle (0 when unfilled).
  std::function<u64(i32 alias)> actor;
  // DIAL topic + speaker handle -> the INFO the speaker says under it (0 if none).
  std::function<u64(u64 dial_topic, u64 speaker)> info;
  // PACK handle -> the actor's travel target world position; false when the
  // package has no resolvable destination. radius is the arrival radius.
  std::function<bool(u64 package, f32 pos[3], f32* radius)> package_target;
};

// Lowers a parsed SCEN into the engine's executable Scene: phases in index
// order, and within each the actions that begin in it, mapped to SceneActions a
// dialogue beat becomes kSayInfo, a travel package becomes kGuideTo (when its
// target resolves), and a timer becomes kWait. Actions that do not resolve
// (unfilled alias, no INFO, no package target) are skipped so a partly-bound
// scene still runs its resolvable beats. Stage advancement is driven separately
// by the scene's Papyrus phase fragments, not encoded here.
Scene CompileScene(const SceneDef& def, const SceneBindings& bindings);

}  // namespace rx::quest

#endif  // RECREATION_QUEST_SCENE_COMPILE_H_
