#ifndef RECREATION_ECS_ENTITY_H_
#define RECREATION_ECS_ENTITY_H_

#include "core/types.h"

namespace rec::ecs {

// Index plus generation. A destroyed slot bumps its generation so stale
// handles can be detected.
struct Entity {
  u32 index = 0xffffffff;
  u32 generation = 0;

  bool operator==(const Entity&) const = default;
  explicit operator bool() const { return index != 0xffffffff; }
};

constexpr Entity kInvalidEntity{};

}  // namespace rec::ecs

#endif  // RECREATION_ECS_ENTITY_H_
