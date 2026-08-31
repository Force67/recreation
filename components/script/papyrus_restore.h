#ifndef RECREATION_SCRIPT_PAPYRUS_RESTORE_H
#define RECREATION_SCRIPT_PAPYRUS_RESTORE_H

// The savegame's Papyrus heap, put back onto live script instances.
//
// This is the only place the two halves meet: components/bethesda reads the
// heap and knows nothing about a VM, components/script runs the VM and knows
// nothing about savegames, and this turns one into the other.
//
// It is not applied in one pass, because it cannot be. Only a quest's own
// scripts exist when the save is applied; a reference's scripts attach when its
// cell streams in, minutes later and over and over as the player moves. So the
// index is held for the whole session and consulted at every attachment (see
// ScriptSystem::set_on_script_restored).

#include <base/containers/unordered_map.h>
#include <base/strings/xstring.h>

#include "components/bethesda/savegame_papyrus.h"
#include "components/script/papyrus/value.h"
#include "core/types.h"

namespace rx {
namespace bethesda {
class FormRemap;
}
}  // namespace rx

namespace rx::script {

namespace papyrus {
class VirtualMachine;
}

class PapyrusRestorer {
 public:
  // Takes the heap over; the save file it came out of is let go long before the
  // last reference that needs it streams in.
  void Build(bethesda::PapyrusHeap heap, const bethesda::FormRemap& remap);

  bool empty() const { return restore_.empty(); }

  // Guest thread only. Writes the save's member variables and state onto an
  // instance that has just attached. True when the save owned this instance,
  // which tells the caller not to raise OnInit over what was just restored.
  bool Apply(papyrus::VirtualMachine& vm,
             papyrus::ObjectRef instance,
             const base::String& script);

  // What the heap holds and how much of it this load order can address. Known
  // as soon as the index is built.
  void LogCoverage() const;
  // What has actually gone back onto live instances. Grows as cells stream in,
  // so this is a snapshot: worth printing once the quest scripts have attached.
  void LogRestored() const;

  const bethesda::PapyrusRestoreStats& stats() const { return stats_; }
  u32 restored_instances() const { return instances_; }
  u32 restored_members() const { return members_; }

 private:
  papyrus::Value ValueFor(papyrus::VirtualMachine& vm, const bethesda::PapyrusValue& value);

  bethesda::PapyrusRestore restore_;
  bethesda::PapyrusRestoreStats stats_;
  // Heap array id -> the VM array built for it. Papyrus arrays are reference
  // values, so two members holding the same saved array must keep holding one
  // array afterwards.
  base::UnorderedMap<u64, u32> arrays_;

  u32 instances_ = 0;
  u32 members_ = 0;
  u32 members_undeclared_ = 0;  // the script here does not declare that member
  u32 states_ = 0;              // instances put back into a non-default state
  u32 refs_dropped_ = 0;        // object values naming something unaddressable
  u32 arrays_built_ = 0;
};

}  // namespace rx::script

#endif  // RECREATION_SCRIPT_PAPYRUS_RESTORE_H
