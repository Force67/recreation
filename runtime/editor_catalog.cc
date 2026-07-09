#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>

#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "bethesda/strings.h"
#include "core/log.h"
#include "editor.h"
#include "engine_context.h"

namespace rx {
namespace {

constexpr int kPerDomainCap = 6000;  // entries per game (keeps the first toggle snappy)
constexpr int kPerTypeCap = 2500;    // per record type, so "All" stays varied

// The placeable base record types, with the browser category each maps to. Only
// types CellStreamer::PlaceObject can resolve a world model for are listed, so
// every catalog entry is actually droppable.
struct TypeBucket {
  u32 type;
  int category;
};
const TypeBucket kPlaceableTypes[] = {
    {FourCc('S', 'T', 'A', 'T'), 1}, {FourCc('M', 'S', 'T', 'T'), 1},
    {FourCc('F', 'U', 'R', 'N'), 2}, {FourCc('D', 'O', 'O', 'R'), 3},
    {FourCc('C', 'O', 'N', 'T'), 4}, {FourCc('F', 'L', 'O', 'R'), 5},
    {FourCc('T', 'R', 'E', 'E'), 5}, {FourCc('L', 'I', 'G', 'H'), 6},
    {FourCc('M', 'I', 'S', 'C'), 7}, {FourCc('B', 'O', 'O', 'K'), 7},
    {FourCc('I', 'N', 'G', 'R'), 7}, {FourCc('A', 'L', 'C', 'H'), 7},
    {FourCc('S', 'L', 'G', 'M'), 7}, {FourCc('K', 'E', 'Y', 'M'), 7},
    {FourCc('W', 'E', 'A', 'P'), 7}, {FourCc('A', 'M', 'M', 'O'), 7},
    {FourCc('A', 'C', 'T', 'I'), 8},
};

// The displayed name: the localized FULL string, falling back to the editor id.
std::string DisplayName(const bethesda::Record& record, const bethesda::StringTable& strings,
                        const std::string& editor_id) {
  const bethesda::Subrecord* full = record.Find(FourCc('F', 'U', 'L', 'L'));
  if (full) {
    if (full->data.size() >= 4) {
      u32 string_id;
      std::memcpy(&string_id, full->data.data(), 4);
      if (const base::String* s = strings.Find(string_id))
        if (s->size() > 0) return std::string(s->c_str());
    }
    std::string literal = record.GetString(FourCc('F', 'U', 'L', 'L'));
    if (!literal.empty()) return literal;
  }
  return editor_id;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Developer/debug forms whose editor id makes them clutter in the palette: view
// models, markers, scratch and test assets. Matched on the lowercased id so the
// browser shows shippable objects, not Creation Kit plumbing. Kept conservative
// (no bare "ref", which is a common legitimate suffix) to avoid dropping real
// assets.
bool IsDeveloperJunk(const std::string& editor_id) {
  if (editor_id.empty()) return false;
  const std::string lid = Lower(editor_id);
  if (lid.rfind("1stperson", 0) == 0) return true;
  static const char* const kFragments[] = {
      "marker", "delete", "dummy", "test", "zzz", "xxx", "debug", "editor", "holding",
  };
  for (const char* frag : kFragments)
    if (lid.find(frag) != std::string::npos) return true;
  return false;
}

}  // namespace

void MapEditor::BuildCatalog() {
  catalog_built_ = true;
  catalog_.clear();
  EnsureDomains();

  const u32 kEdid = FourCc('E', 'D', 'I', 'D');
  const u32 kModl = FourCc('M', 'O', 'D', 'L');
  // One game at a time, so a Fallout 4 prop and a Skyrim prop with the same name
  // both appear (the dedupe set is per domain).
  for (int domain = 0; domain < static_cast<int>(domains_.size()); ++domain) {
    const EditorPlaceDomain& dom = domains_[domain];
    if (!dom.records || !dom.strings) continue;
    const bethesda::RecordStore& records = *dom.records;
    const bethesda::StringTable& strings = *dom.strings;
    const int domain_start = static_cast<int>(catalog_.size());

    std::unordered_set<std::string> seen;  // collapse rows that read identically
    for (const TypeBucket& tb : kPlaceableTypes) {
      if (static_cast<int>(catalog_.size()) - domain_start >= kPerDomainCap) break;
      int taken = 0;
      records.EachOfType(
          tb.type, [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
            if (taken >= kPerTypeCap ||
                static_cast<int>(catalog_.size()) - domain_start >= kPerDomainCap) {
              return;
            }
            bethesda::Record record;
            if (!records.Parse(id, &record)) return;
            // Only forms with a world model are droppable.
            const bethesda::Subrecord* modl = record.Find(kModl);
            if (!modl || modl->data.empty()) return;
            std::string editor_id = record.GetString(kEdid);
            if (IsDeveloperJunk(editor_id)) return;
            std::string name = DisplayName(record, strings, editor_id);
            if (name.empty()) return;  // nameless and idless: not useful to browse
            std::string key = Lower(name);
            key.push_back('\x1f');
            key.append(reinterpret_cast<const char*>(&tb.type), sizeof(tb.type));
            if (!seen.insert(std::move(key)).second) return;  // duplicate display row
            CatalogEntry e;
            e.base = id;
            e.type = tb.type;
            e.category = tb.category;
            e.domain = domain;
            e.editor_id = std::move(editor_id);
            e.name = std::move(name);
            catalog_.push_back(std::move(e));
            ++taken;
          });
    }
  }

  // Group by category, then by game, then float entries with a real FULL name
  // above id-only ones, then sort by name; the filter keeps this order.
  std::sort(catalog_.begin(), catalog_.end(), [](const CatalogEntry& a, const CatalogEntry& b) {
    if (a.category != b.category) return a.category < b.category;
    if (a.domain != b.domain) return a.domain < b.domain;
    const bool a_named = a.name != a.editor_id;
    const bool b_named = b.name != b.editor_id;
    if (a_named != b_named) return a_named;
    return Lower(a.name) < Lower(b.name);
  });
  RX_INFO("editor catalog: {} curated placeable forms across {} game(s)", catalog_.size(),
           domains_.size());
  RefreshFilter();
}

void MapEditor::RefreshFilter() {
  filtered_.clear();
  const std::string needle = Lower(search_);
  for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
    const CatalogEntry& e = catalog_[i];
    if (category_ != 0 && e.category != category_) continue;
    if (!needle.empty()) {
      // Match the name, the editor id, or the game (so "fallout" narrows to that
      // game's assets in a multi-game session).
      const bool game_match = domains_.size() > 1 && e.domain >= 0 &&
                              e.domain < static_cast<int>(domains_.size()) &&
                              Lower(domains_[e.domain].name).find(needle) != std::string::npos;
      if (!game_match && Lower(e.name).find(needle) == std::string::npos &&
          Lower(e.editor_id).find(needle) == std::string::npos) {
        continue;
      }
    }
    filtered_.push_back(i);
  }
  if (page_first_ >= static_cast<int>(filtered_.size())) page_first_ = 0;
}

}  // namespace rx
