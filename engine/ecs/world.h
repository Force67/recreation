#ifndef RECREATION_ECS_WORLD_H_
#define RECREATION_ECS_WORLD_H_

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "ecs/archetype.h"
#include "ecs/component.h"
#include "ecs/entity.h"

namespace rec::ecs {

class World {
 public:
  World();
  ~World();

  Entity Create();
  void Destroy(Entity entity);
  bool IsAlive(Entity entity) const;

  template <typename T>
  T& Add(Entity entity, T value) {
    void* slot = AddRaw(entity, GetComponentId<T>());
    return *new (slot) T(std::move(value));
  }

  template <typename T>
  void Remove(Entity entity) {
    RemoveRaw(entity, GetComponentId<T>());
  }

  template <typename T>
  T* Get(Entity entity) {
    return static_cast<T*>(GetRaw(entity, GetComponentId<T>()));
  }

  template <typename T>
  bool Has(Entity entity) const {
    return HasRaw(entity, GetComponentId<T>());
  }

  // Calls fn(Entity, Ts&...) for every entity that has all of Ts.
  template <typename... Ts, typename Fn>
  void Each(Fn&& fn) {
    Signature required = MakeSignature({GetComponentId<Ts>()...});
    for (auto& archetype : archetypes_) {
      if (!SignatureContainsAll(archetype->signature(), required)) continue;
      auto columns = std::make_tuple(static_cast<Ts*>(archetype->ColumnData(GetComponentId<Ts>()))...);
      for (u32 row = 0; row < archetype->row_count(); ++row) {
        fn(archetype->entity_at(row), std::get<Ts*>(columns)[row]...);
      }
    }
  }

  size_t entity_count() const { return live_count_; }

  // Untyped access, used by replication and converters.
  void* AddRaw(Entity entity, ComponentId id);
  void RemoveRaw(Entity entity, ComponentId id);
  void* GetRaw(Entity entity, ComponentId id);
  bool HasRaw(Entity entity, ComponentId id) const;

 private:
  struct EntityRecord {
    Archetype* archetype = nullptr;
    u32 row = 0;
    u32 generation = 0;
    bool alive = false;
  };

  Archetype* GetOrCreateArchetype(const Signature& signature);
  void MoveEntity(Entity entity, EntityRecord& record, Archetype* destination);

  std::vector<EntityRecord> records_;
  std::vector<u32> free_indices_;
  std::vector<std::unique_ptr<Archetype>> archetypes_;
  std::map<Signature, Archetype*> archetype_lookup_;
  size_t live_count_ = 0;
};

}  // namespace rec::ecs

#endif  // RECREATION_ECS_WORLD_H_
