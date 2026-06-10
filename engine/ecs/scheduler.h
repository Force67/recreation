#ifndef RECREATION_ECS_SCHEDULER_H_
#define RECREATION_ECS_SCHEDULER_H_

#include <functional>
#include <string>
#include <vector>

#include "core/types.h"
#include "ecs/world.h"

namespace rec::ecs {

// Systems are plain functions. No base classes, no virtual dispatch.
using SystemFn = std::function<void(World& world, f32 dt)>;

enum class Stage { kPreSim, kSim, kPostSim, kPreRender, kStageCount };

class Scheduler {
 public:
  void AddSystem(Stage stage, std::string name, SystemFn fn);
  void RunStage(Stage stage, World& world, f32 dt);

 private:
  struct System {
    std::string name;
    SystemFn fn;
  };

  std::vector<System> stages_[static_cast<size_t>(Stage::kStageCount)];
};

}  // namespace rec::ecs

#endif  // RECREATION_ECS_SCHEDULER_H_
