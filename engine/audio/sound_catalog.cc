#include "audio/sound_catalog.h"

#include <cctype>
#include <cstring>

#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "core/log.h"

namespace rec::audio {
namespace {

constexpr u32 kSndr = FourCc('S', 'N', 'D', 'R');
constexpr u32 kSoun = FourCc('S', 'O', 'U', 'N');
constexpr u32 kAnam = FourCc('A', 'N', 'A', 'M');
constexpr u32 kFnam = FourCc('F', 'N', 'A', 'M');
constexpr u32 kSdsc = FourCc('S', 'D', 'S', 'C');

// Lowercases, turns backslashes into forward slashes, and roots the path under
// "sound/" (Bethesda's sound file references are relative to Data\Sound\). Empty
// in, empty out.
std::string NormalizeSoundPath(std::string_view raw) {
  std::string out;
  out.reserve(raw.size() + 6);
  for (char c : raw) {
    if (c == '\0') break;
    if (c == '\\') c = '/';
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (out.empty()) return out;
  if (out.rfind("sound/", 0) != 0) out = "sound/" + out;
  return out;
}

// The first file path on a SNDR descriptor (its ANAM variations), or empty.
std::string SndrPath(const bethesda::Record& record) {
  for (const bethesda::Subrecord& sub : record.subrecords) {
    if (sub.type != kAnam || sub.data.empty()) continue;
    return NormalizeSoundPath(
        std::string_view(reinterpret_cast<const char*>(sub.data.data()), sub.data.size()));
  }
  return {};
}

}  // namespace

void SoundCatalog::Build(const bethesda::RecordStore& records) {
  paths_.clear();

  // Sound descriptors first, so a SOUN that links one (SDSC) resolves in one pass.
  records.EachOfType(kSndr, [&](bethesda::GlobalFormId id,
                                const bethesda::RecordStore::StoredRecord&) {
    bethesda::Record record;
    if (!records.Parse(id, &record)) return;
    std::string path = SndrPath(record);
    if (!path.empty()) paths_[id.packed()] = std::move(path);
  });

  records.EachOfType(kSoun, [&](bethesda::GlobalFormId id,
                                const bethesda::RecordStore::StoredRecord& stored) {
    bethesda::Record record;
    if (!records.Parse(id, &record)) return;
    // Modern SOUN: a link to a sound descriptor whose path we resolved above.
    if (const bethesda::Subrecord* sdsc = record.Find(kSdsc); sdsc && sdsc->data.size() >= 4) {
      u32 raw;
      std::memcpy(&raw, sdsc->data.data(), 4);
      const bethesda::GlobalFormId descriptor =
          records.ResolveFrom(bethesda::RawFormId{raw}, stored.winning_plugin);
      if (auto it = paths_.find(descriptor.packed()); it != paths_.end()) {
        paths_[id.packed()] = it->second;
        return;
      }
    }
    // Legacy SOUN: the filename is stored directly.
    const std::string fnam = record.GetString(kFnam);
    if (!fnam.empty()) paths_[id.packed()] = NormalizeSoundPath(fnam);
  });

  REC_INFO("audio: sound catalog built, {} sound forms", paths_.size());
}

std::string SoundCatalog::PathFor(bethesda::GlobalFormId form) const {
  auto it = paths_.find(form.packed());
  return it != paths_.end() ? it->second : std::string{};
}

}  // namespace rec::audio
