#ifndef RECREATION_ECS_COMPONENT_H_
#define RECREATION_ECS_COMPONENT_H_

#include <type_traits>

#include "core/types.h"

namespace rec::ecs {

using ComponentId = u32;

struct ComponentInfo {
  u32 size = 0;
  u32 align = 0;
  void (*move_construct)(void* dst, void* src) = nullptr;
  void (*destruct)(void* ptr) = nullptr;
};

namespace detail {

ComponentId NextComponentId();
void RegisterComponent(ComponentId id, const ComponentInfo& info);

template <typename T>
ComponentId ComponentIdFor() {
  static_assert(std::is_move_constructible_v<T>);
  static const ComponentId id = [] {
    ComponentId new_id = NextComponentId();
    RegisterComponent(new_id, ComponentInfo{
      .size = sizeof(T),
      .align = alignof(T),
      .move_construct = [](void* dst, void* src) { new (dst) T(std::move(*static_cast<T*>(src))); },
      .destruct = [](void* ptr) { static_cast<T*>(ptr)->~T(); },
    });
    return new_id;
  }();
  return id;
}

}  // namespace detail

template <typename T>
ComponentId GetComponentId() {
  return detail::ComponentIdFor<std::remove_cvref_t<T>>();
}

const ComponentInfo& GetComponentInfo(ComponentId id);

}  // namespace rec::ecs

#endif  // RECREATION_ECS_COMPONENT_H_
