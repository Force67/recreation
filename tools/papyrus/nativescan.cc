// nativescan: enumerate every native function the base-game scripts declare and
// report how much of that surface the engine's native registry handles.
//
//   nativescan <data_dir> [--list-missing]
//
// Scans scripts/*.pex across the mounted archives, collects every function
// flagged native, and checks each against the Skyrim native table. Grounds the
// "every script fn handler" goal in the real, finite set the game ships.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "bethesda/archive.h"
#include "script/games/skyrim/skyrim_natives.h"
#include "script/papyrus/pex.h"
#include "script/papyrus_guest.h"

namespace {

using namespace rec;
using namespace rec::script::papyrus;

std::string Lower(std::string s) {
  std::ranges::transform(s, s.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
  return s;
}

void CollectNatives(const PexFile& pex, std::map<std::string, std::set<std::string>>& by_type) {
  for (const Object& obj : pex.objects) {
    std::string type = pex.Str(obj.name);
    for (const State& st : obj.states)
      for (const NamedFunction& nf : st.functions)
        if (nf.function.is_native) by_type[type].insert(pex.Str(nf.name));
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <data_dir> [--list-missing]\n", argv[0]);
    return 2;
  }
  std::string data_dir = argv[1];
  bool list_missing = argc > 2 && std::string(argv[2]) == "--list-missing";

  std::map<std::string, std::set<std::string>> by_type;  // script type -> native fn names
  int scripts_scanned = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
    auto provider = bethesda::OpenArchive(entry.path().string());
    if (!provider) continue;
    std::set<std::string> script_paths;
    provider->Enumerate([&](std::string_view path) {
      if (path.starts_with("scripts/") && path.ends_with(".pex"))
        script_paths.emplace(path);
    });
    for (const std::string& path : script_paths) {
      auto blob = provider->Read(path);
      if (!blob) continue;
      PexFile pex;
      if (!ParsePex(ByteSpan(blob->data(), blob->size()), &pex)) continue;
      CollectNatives(pex, by_type);
      ++scripts_scanned;
    }
  }

  // The native table the engine ships (timers/debug from the guest, plus the
  // Skyrim surface).
  rec::script::PapyrusGuest guest(bethesda::Game::kSkyrimSe);
  rec::script::skyrim::RegisterSkyrimNatives(guest.natives(), nullptr);
  const NativeRegistry& reg = guest.natives();

  int total = 0;
  int handled = 0;
  std::map<std::string, std::pair<int, int>> per_type;  // type -> {handled, total}
  for (const auto& [type, fns] : by_type)
    for (const std::string& fn : fns) {
      ++total;
      bool ok = reg.Find(type, fn) != nullptr;
      if (ok) ++handled;
      per_type[type].second++;
      if (ok) per_type[type].first++;
      if (list_missing && !ok) std::printf("MISSING %s.%s\n", type.c_str(), fn.c_str());
    }

  std::printf("scanned %d scripts, %zu script types declare natives\n", scripts_scanned,
              by_type.size());
  std::printf("native functions: %d total, %d handled (%.1f%%)\n", total, handled,
              total ? 100.0 * handled / total : 0.0);

  // The script types with the most native surface, and how much is covered.
  std::vector<std::pair<std::string, std::pair<int, int>>> ranked(per_type.begin(), per_type.end());
  std::ranges::sort(ranked, [](const auto& a, const auto& b) {
    return a.second.second > b.second.second;
  });
  std::printf("top native-declaring types (handled/total):\n");
  for (size_t i = 0; i < ranked.size() && i < 15; ++i)
    std::printf("  %-24s %d/%d\n", ranked[i].first.c_str(), ranked[i].second.first,
                ranked[i].second.second);
  return 0;
}
