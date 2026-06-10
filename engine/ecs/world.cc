#include "ecs/world.h"

#include <atomic>

namespace rec::ecs {

namespace detail {
namespace {
std::atomic<ComponentId> g_next_id{0};
std::vector<ComponentInfo> g_infos(256);
}  // namespace

ComponentId NextComponentId() { return g_next_id.fetch_add(1); }

void RegisterComponent(ComponentId id, const ComponentInfo& info) {
  if (id >= g_infos.size()) g_infos.resize(id + 1);
  g_infos[id] = info;
}
}  // namespace detail

const ComponentInfo& GetComponentInfo(ComponentId id) { return detail::g_infos[id]; }

World::World() { GetOrCreateArchetype({}); }

World::~World() = default;

Entity World::Create() {
  u32 index;
  if (!free_indices_.empty()) {
    index = free_indices_.back();
    free_indices_.pop_back();
  } else {
    index = static_cast<u32>(records_.size());
    records_.emplace_back();
  }
  EntityRecord& record = records_[index];
  record.alive = true;
  Entity entity{index, record.generation};
  record.archetype = archetypes_.front().get();
  record.row = record.archetype->AddRow(entity);
  ++live_count_;
  return entity;
}

void World::Destroy(Entity entity) {
  if (!IsAlive(entity)) return;
  EntityRecord& record = records_[entity.index];
  Entity moved = record.archetype->SwapRemoveRow(record.row);
  if (moved) records_[moved.index].row = record.row;
  record.alive = false;
  record.archetype = nullptr;
  ++record.generation;
  free_indices_.push_back(entity.index);
  --live_count_;
}

bool World::IsAlive(Entity entity) const {
  return entity.index < records_.size() && records_[entity.index].alive &&
         records_[entity.index].generation == entity.generation;
}

void* World::AddRaw(Entity entity, ComponentId id) {
  EntityRecord& record = records_[entity.index];
  if (void* existing = record.archetype->ComponentAt(id, record.row)) {
    GetComponentInfo(id).destruct(existing);
    return existing;
  }
  Signature signature = record.archetype->signature();
  signature.insert(std::lower_bound(signature.begin(), signature.end(), id), id);
  MoveEntity(entity, record, GetOrCreateArchetype(signature));
  return record.archetype->ComponentAt(id, record.row);
}

void World::RemoveRaw(Entity entity, ComponentId id) {
  EntityRecord& record = records_[entity.index];
  if (!SignatureContains(record.archetype->signature(), id)) return;
  Signature signature = record.archetype->signature();
  signature.erase(std::lower_bound(signature.begin(), signature.end(), id));
  MoveEntity(entity, record, GetOrCreateArchetype(signature));
}

void* World::GetRaw(Entity entity, ComponentId id) {
  if (!IsAlive(entity)) return nullptr;
  EntityRecord& record = records_[entity.index];
  return record.archetype->ComponentAt(id, record.row);
}

bool World::HasRaw(Entity entity, ComponentId id) const {
  if (!IsAlive(entity)) return false;
  const EntityRecord& record = records_[entity.index];
  return SignatureContains(record.archetype->signature(), id);
}

Archetype* World::GetOrCreateArchetype(const Signature& signature) {
  auto it = archetype_lookup_.find(signature);
  if (it != archetype_lookup_.end()) return it->second;
  archetypes_.push_back(std::make_unique<Archetype>(signature));
  Archetype* archetype = archetypes_.back().get();
  archetype_lookup_.emplace(signature, archetype);
  return archetype;
}

void World::MoveEntity(Entity entity, EntityRecord& record, Archetype* destination) {
  Archetype* source = record.archetype;
  u32 source_row = record.row;
  u32 destination_row = destination->AddRow(entity);
  for (ComponentId id : destination->signature()) {
    void* from = source->ComponentAt(id, source_row);
    if (!from) continue;
    GetComponentInfo(id).move_construct(destination->ComponentAt(id, destination_row), from);
  }
  Entity moved = source->SwapRemoveRow(source_row);
  if (moved) records_[moved.index].row = source_row;
  record.archetype = destination;
  record.row = destination_row;
}

}  // namespace rec::ecs
