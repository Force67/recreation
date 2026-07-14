#ifndef RECREATION_WORLD_PROP_STREAMING_H_
#define RECREATION_WORLD_PROP_STREAMING_H_

#include "core/types.h"

namespace rx::world {

enum PropCapability : u32 {
  kPropNone = 0,
  kPropActivatable = 1u << 0,
  kPropDoor = 1u << 1,
  kPropContainer = 1u << 2,
  kPropScripted = 1u << 3,
  kPropPhysics = 1u << 4,
};

struct PropTraits {
  u32 base_type = 0;
  bool placed_script = false;
  bool base_script = false;
  bool primitive = false;
  bool teleport = false;
  bool stateful = false;
};

struct PropClassification {
  u32 capabilities = kPropNone;
  bool batchable = false;
};

// Pure policy for selecting the compact static representation. Stateful or
// addressable references stay in ECS; only inert architecture/vegetation may
// become a cell-owned instance slot.
PropClassification ClassifyProp(const PropTraits& traits);

// Stable synthetic form handle for one instantiated pack-in child. It uses the
// otherwise-impractical high half of the plugin namespace and is independent of
// cell traversal/insertion order, so server and clients derive the same handle.
u64 PackInChildHandle(u64 parent, u64 source_child);

}  // namespace rx::world

#endif  // RECREATION_WORLD_PROP_STREAMING_H_
