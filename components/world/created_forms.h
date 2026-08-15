#ifndef RECREATION_WORLD_CREATED_FORMS_H_
#define RECREATION_WORLD_CREATED_FORMS_H_

// Base forms a savegame invented: the potions and poisons the player brewed and
// the enchantments they made.
//
// No plugin holds a record for one, so nothing that answers questions off the
// RecordStore can answer them here: what the thing is called, what it weighs and
// what it is worth exist only in the save's own created-object table. This is
// where that table lands, keyed by the synthetic handle the savegame reader
// gives each entry (bethesda::kCreatedFormPlugin), so the inventory, the item
// catalogue and the HUD can look one up exactly where they would have looked up
// a record.
//
// Deliberately not a record: the store answers the three questions an item has
// to answer to be carried, and nothing else. A brewed potion has no model of its
// own in the game either -- the game picks one from the effect -- so there is no
// mesh here to find.

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "components/bethesda/form_id.h"
#include "core/types.h"

namespace rx::world {

struct CreatedForm {
  base::String name;   // "Potion of Healing", built from the strongest effect
  f32 weight = 0.0f;
  u32 value = 0;       // gold
  bool consumable = false;  // a potion or poison, as opposed to an enchantment
};

class CreatedForms {
 public:
  void Add(bethesda::GlobalFormId id, CreatedForm form) {
    forms_[id.packed()] = base::move(form);
  }
  const CreatedForm* Find(bethesda::GlobalFormId id) const { return forms_.find(id.packed()); }
  mem_size size() const { return forms_.size(); }

 private:
  base::UnorderedMap<u64, CreatedForm> forms_;
};

}  // namespace rx::world

#endif  // RECREATION_WORLD_CREATED_FORMS_H_
