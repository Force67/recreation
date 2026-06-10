#ifndef RECREATION_ECS_ARCHETYPE_H_
#define RECREATION_ECS_ARCHETYPE_H_

#include <algorithm>
#include <vector>

#include "ecs/component.h"
#include "ecs/entity.h"

namespace rec::ecs {

// Sorted set of component ids identifying an archetype.
using Signature = std::vector<ComponentId>;

inline Signature MakeSignature(std::initializer_list<ComponentId> ids) {
  Signature sig(ids);
  std::sort(sig.begin(), sig.end());
  return sig;
}

inline bool SignatureContains(const Signature& sig, ComponentId id) {
  return std::binary_search(sig.begin(), sig.end(), id);
}

inline bool SignatureContainsAll(const Signature& sig, const Signature& subset) {
  return std::includes(sig.begin(), sig.end(), subset.begin(), subset.end());
}

// Columnar storage. One column per component type, rows are entities.
class Archetype {
 public:
  explicit Archetype(Signature signature);
  ~Archetype();

  Archetype(const Archetype&) = delete;
  Archetype& operator=(const Archetype&) = delete;

  // Appends a row with uninitialized component memory, returns the row index.
  u32 AddRow(Entity entity);

  // Swap removes a row, destructing its components. Returns the entity that
  // was moved into the vacated row, or kInvalidEntity if the last row was removed.
  Entity SwapRemoveRow(u32 row);

  void* ComponentAt(ComponentId id, u32 row);
  void* ColumnData(ComponentId id);
  int ColumnIndex(ComponentId id) const;

  const Signature& signature() const { return signature_; }
  u32 row_count() const { return static_cast<u32>(entities_.size()); }
  Entity entity_at(u32 row) const { return entities_[row]; }

 private:
  struct Column {
    ComponentId id;
    u32 stride;
    std::vector<u8> data;
  };

  Signature signature_;
  std::vector<Column> columns_;
  std::vector<Entity> entities_;
};

}  // namespace rec::ecs

#endif  // RECREATION_ECS_ARCHETYPE_H_
