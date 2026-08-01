#ifndef RECREATION_SCRIPT_PAPYRUS_NATIVE_H_
#define RECREATION_SCRIPT_PAPYRUS_NATIVE_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/functional/function.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/script/papyrus/value.h"

namespace rx::script::papyrus {

class VirtualMachine;

// A native (engine-implemented) Papyrus function. self is None for global
// functions. args are the call arguments; the return is the function's result
// (None for void). Natives reach the engine through the VM's game bindings.
using NativeFunction =
    base::Function<Value(VirtualMachine& vm, ObjectRef self, base::Vector<Value>& args)>;

// The per-game native surface: a flat table keyed by script type + function
// name. This is the one component that differs between Skyrim, Fallout 4 and
// 76; the VM core stays game-agnostic and only consults this table when a
// resolved function is marked native. Lookups are case-insensitive, matching
// Papyrus name resolution.
class NativeRegistry {
 public:
  void Register(base::StringRef script_type, base::StringRef function, NativeFunction fn);
  const NativeFunction* Find(base::StringRef script_type, base::StringRef function) const;

  size_t size() const { return table_.size(); }

 private:
  static base::String Key(base::StringRef script_type, base::StringRef function);
  base::UnorderedMap<base::String, NativeFunction> table_;
};

}  // namespace rx::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_NATIVE_H_
