#ifndef RECREATION_NET_REPLICATION_H_
#define RECREATION_NET_REPLICATION_H_

#include <functional>
#include <vector>

#include "recreation/core/types.h"
#include "recreation/ecs/world.h"

namespace rec::net {

// Stable identity across machines. Local Entity values differ per peer, the
// NetworkId is what snapshots address.
struct NetworkId {
  u64 value = 0;
};

// Components opt into replication with a serializer pair. The registry walks
// replicated entities each tick and writes dirty component data into the
// outgoing snapshot.
class ReplicationRegistry {
 public:
  struct Entry {
    ecs::ComponentId component;
    std::function<void(const void* component_ptr, std::vector<u8>* out)> write;
    std::function<void(void* component_ptr, ByteSpan in)> read;
  };

  template <typename T>
  void Register(std::function<void(const T&, std::vector<u8>*)> write,
                std::function<void(T&, ByteSpan)> read) {
    entries_.push_back(Entry{
        .component = ecs::GetComponentId<T>(),
        .write = [w = std::move(write)](const void* ptr, std::vector<u8>* out) {
          w(*static_cast<const T*>(ptr), out);
        },
        .read = [r = std::move(read)](void* ptr, ByteSpan in) { r(*static_cast<T*>(ptr), in); },
    });
  }

  // Serializes all replicated components of entities that carry NetworkId.
  void WriteSnapshot(ecs::World& world, std::vector<u8>* out) const;
  void ApplySnapshot(ecs::World& world, ByteSpan snapshot) const;

 private:
  std::vector<Entry> entries_;
};

}  // namespace rec::net

#endif  // RECREATION_NET_REPLICATION_H_
