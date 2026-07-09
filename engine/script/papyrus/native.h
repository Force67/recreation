#ifndef RECREATION_SCRIPT_PAPYRUS_NATIVE_H_
#define RECREATION_SCRIPT_PAPYRUS_NATIVE_H_

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "script/papyrus/value.h"

namespace rx::script::papyrus {

class VirtualMachine;

// A native (engine-implemented) Papyrus function. self is None for global
// functions. args are the call arguments; the return is the function's result
// (None for void). Natives reach the engine through the VM's game bindings.
using NativeFunction =
    std::function<Value(VirtualMachine& vm, ObjectRef self, std::vector<Value>& args)>;

// The per-game native surface: a flat table keyed by script type + function
// name. This is the one component that differs between Skyrim, Fallout 4 and
// 76; the VM core stays game-agnostic and only consults this table when a
// resolved function is marked native. Lookups are case-insensitive, matching
// Papyrus name resolution.
class NativeRegistry {
 public:
  void Register(std::string_view script_type, std::string_view function, NativeFunction fn);
  const NativeFunction* Find(std::string_view script_type, std::string_view function) const;

  size_t size() const { return table_.size(); }

 private:
  static std::string Key(std::string_view script_type, std::string_view function);
  std::unordered_map<std::string, NativeFunction> table_;
};

}  // namespace rx::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_NATIVE_H_
