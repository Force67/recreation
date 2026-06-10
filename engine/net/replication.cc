#include "net/replication.h"

namespace rec::net {

void ReplicationRegistry::WriteSnapshot(ecs::World& world, std::vector<u8>* out) const {
  world.Each<NetworkId>([&](ecs::Entity entity, NetworkId& id) {
    for (const Entry& entry : entries_) {
      void* component = world.GetRaw(entity, entry.component);
      if (!component) continue;
      // TODO: frame the stream (network id, component id, payload size),
      // delta against the last acked snapshot, quantize transforms.
      entry.write(component, out);
    }
  });
}

void ReplicationRegistry::ApplySnapshot(ecs::World& world, ByteSpan snapshot) const {
  // TODO: parse frames, look up or spawn the entity for each network id,
  // apply payloads through Entry::read, interpolate remote transforms.
}

}  // namespace rec::net
