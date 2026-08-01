#include "components/script/papyrus/native.h"

#include <base/memory/move.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include <cctype>

namespace rx::script::papyrus {

base::String NativeRegistry::Key(base::StringRef script_type, base::StringRef function) {
  base::String key;
  key.reserve(script_type.size() + 1 + function.size());
  auto lower = [&](base::StringRef s) {
    for (char c : s)
      key.push_back(static_cast<char>(std::tolower((unsigned char)c)));
  };
  lower(script_type);
  key.push_back('.');
  lower(function);
  return key;
}

void NativeRegistry::Register(base::StringRef script_type,
                              base::StringRef function,
                              NativeFunction fn) {
  table_[Key(script_type, function)] = base::move(fn);
}

const NativeFunction* NativeRegistry::Find(base::StringRef script_type,
                                           base::StringRef function) const {
  auto* it = table_.find(Key(script_type, function));
  return it == nullptr ? nullptr : &*it;
}

}  // namespace rx::script::papyrus
