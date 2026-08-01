// nativescan: enumerate every native function the base-game scripts declare and
// report how much of that surface the engine's native registry handles.
//
//   nativescan <data_dir> [--list-missing]
//
// Scans scripts/*.pex across the mounted archives, collects every function
// flagged native, and checks each against the Skyrim native table. Grounds the
// "every script fn handler" goal in the real, finite set the game ships.

#include <base/algorithm.h>
#include <base/containers/map.h>
#include <base/containers/pair.h>
#include <base/containers/set.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <filesystem>

#include "components/bethesda/archive.h"
#include "components/script/games/skyrim/skyrim_natives.h"
#include "components/script/papyrus/pex.h"
#include "components/script/papyrus_guest.h"

namespace {

using namespace rx;
// rx::u64/i64 (long) and base/arch.h's (long long) are different types sharing
// a global name, so the 64-bit spellings below are qualified; the other scalars
// agree between the two and need no help.
using namespace rx::script::papyrus;

base::String Lower(base::String s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void CollectNatives(const PexFile& pex, base::Map<base::String, base::Set<base::String>>& by_type) {
  for (const Object& obj : pex.objects) {
    base::String type = pex.Str(obj.name);
    for (const State& st : obj.states)
      for (const NamedFunction& nf : st.functions)
        if (nf.function.is_native)
          by_type[type].insert(pex.Str(nf.name));
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <data_dir> [--list-missing]\n", argv[0]);
    return 2;
  }
  base::String data_dir = argv[1];
  bool list_missing = argc > 2 && base::String(argv[2]) == "--list-missing";

  base::Map<base::String, base::Set<base::String>> by_type;  // script type -> native fn names
  int scripts_scanned = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir.c_str(), ec)) {
    auto provider = bethesda::OpenArchive(entry.path().string());
    if (!provider)
      continue;
    base::Set<base::String> script_paths;
    provider->Enumerate([&](base::StringRef path) {
      if (path.starts_with("scripts/") && path.ends_with(".pex"))
        script_paths.insert(base::String(path));
    });
    for (const base::String& path : script_paths) {
      auto blob = provider->Read(path);
      if (!blob)
        continue;
      PexFile pex;
      if (!ParsePex(ByteSpan(blob->data(), blob->size()), &pex))
        continue;
      CollectNatives(pex, by_type);
      ++scripts_scanned;
    }
  }

  // The native table the engine ships (timers/debug from the guest, plus the
  // Skyrim surface).
  rx::script::PapyrusGuest guest(bethesda::Game::kSkyrimSe);
  rx::script::skyrim::RegisterSkyrimNatives(guest.natives(), nullptr);
  const NativeRegistry& reg = guest.natives();

  int total = 0;
  int handled = 0;
  base::Map<base::String, base::Pair<int, int>> per_type;  // type -> {handled, total}
  for (const auto& [type, fns] : by_type)
    for (const base::String& fn : fns) {
      ++total;
      bool ok = reg.Find(type, fn) != nullptr;
      if (ok)
        ++handled;
      per_type[type].second++;
      if (ok)
        per_type[type].first++;
      if (list_missing && !ok)
        std::printf("MISSING %s.%s\n", type.c_str(), fn.c_str());
    }

  std::printf("scanned %d scripts, %zu script types declare natives\n", scripts_scanned,
              by_type.size());
  std::printf("native functions: %d total, %d handled (%.1f%%)\n", total, handled,
              total ? 100.0 * handled / total : 0.0);

  // The script types with the most native surface, and how much is covered.
  base::Vector<base::Pair<base::String, base::Pair<int, int>>> ranked;
  for (const auto& entry : per_type)
    ranked.push_back({entry.first, entry.second});
  base::Sort(ranked.begin(), ranked.end(),
             [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
  std::printf("top native-declaring types (handled/total):\n");
  for (size_t i = 0; i < ranked.size() && i < 15; ++i)
    std::printf("  %-24s %d/%d\n", ranked[i].first.c_str(), ranked[i].second.first,
                ranked[i].second.second);
  return 0;
}
