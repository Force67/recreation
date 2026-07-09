#include "script/papyrus/native.h"

#include <algorithm>
#include <cctype>

namespace rx::script::papyrus {

std::string NativeRegistry::Key(std::string_view script_type, std::string_view function) {
  std::string key;
  key.reserve(script_type.size() + 1 + function.size());
  auto lower = [&](std::string_view s) {
    for (char c : s) key.push_back(static_cast<char>(std::tolower((unsigned char)c)));
  };
  lower(script_type);
  key.push_back('.');
  lower(function);
  return key;
}

void NativeRegistry::Register(std::string_view script_type, std::string_view function,
                              NativeFunction fn) {
  table_[Key(script_type, function)] = std::move(fn);
}

const NativeFunction* NativeRegistry::Find(std::string_view script_type,
                                           std::string_view function) const {
  auto it = table_.find(Key(script_type, function));
  return it == table_.end() ? nullptr : &it->second;
}

}  // namespace rx::script::papyrus
