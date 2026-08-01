#include "components/audio/sound_catalog.h"

#include <base/memory/move.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include <cctype>
#include <cstring>

#include "components/bethesda/load_order.h"
#include "components/bethesda/record.h"
#include "core/log.h"

namespace rx::audio {
namespace {

constexpr u32 kSndr = FourCc('S', 'N', 'D', 'R');
constexpr u32 kSoun = FourCc('S', 'O', 'U', 'N');
constexpr u32 kAnam = FourCc('A', 'N', 'A', 'M');
constexpr u32 kFnam = FourCc('F', 'N', 'A', 'M');
constexpr u32 kSdsc = FourCc('S', 'D', 'S', 'C');

// Lowercases, turns backslashes into forward slashes, and roots the path under
// "sound/" (Bethesda's sound file references are relative to Data\Sound\). Empty
// in, empty out. The ANAM/FNAM references are inconsistent across records: some
// store the bare "fx\..." path, some "sound\fx\...", and some the full
// "data\sound\fx\..."; strip a leading "data/" and prepend "sound/" only when it
// is not already rooted there, so every form resolves to the one Vfs path.
base::String NormalizeSoundPath(base::StringRef raw) {
  base::String out;
  out.reserve(raw.size() + 6);
  for (char c : raw) {
    if (c == '\0') break;
    if (c == '\\') c = '/';
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (out.empty()) return out;
  // Some references carry a leading separator ("\data\sound\..."), so trim any
  // leading slashes before the data/ + sound/ rooting below, or they would slip
  // past both checks and produce "sound//data/sound/...".
  if (const size_t lead = out.find_first_not_of('/'); lead == base::String::npos)
    return {};
  else if (lead > 0)
    out.erase(0, lead);
  if (out.rfind("data/", 0) == 0) out.erase(0, 5);
  if (out.rfind("sound/", 0) != 0) out = "sound/" + out;
  return out;
}

// The first file path on a SNDR descriptor (its ANAM variations), or empty.
base::String SndrPath(const bethesda::Record& record) {
  for (const bethesda::Subrecord& sub : record.subrecords) {
    if (sub.type != kAnam || sub.data.empty()) continue;
    return NormalizeSoundPath(
        base::StringRef(reinterpret_cast<const char*>(sub.data.data()), sub.data.size()));
  }
  return {};
}

}  // namespace

void SoundCatalog::Build(const bethesda::RecordStore& records) {
  paths_.clear();

  // Sound descriptors first, so a SOUN that links one (SDSC) resolves in one pass.
  records.EachOfType(kSndr,
                     [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
                       bethesda::Record record;
                       if (!records.Parse(id, &record)) return;
                       base::String path = SndrPath(record);
                       if (!path.empty()) paths_[id.packed()] = base::move(path);
                     });

  records.EachOfType(
      kSoun, [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord& stored) {
        bethesda::Record record;
        if (!records.Parse(id, &record)) return;
        // Modern SOUN: a link to a sound descriptor whose path we resolved above.
        if (const bethesda::Subrecord* sdsc = record.Find(kSdsc); sdsc && sdsc->data.size() >= 4) {
          u32 raw;
          std::memcpy(&raw, sdsc->data.data(), 4);
          const bethesda::GlobalFormId descriptor =
              records.ResolveFrom(bethesda::RawFormId{raw}, stored.winning_plugin);
          if (const base::String* path = paths_.find(descriptor.packed())) {
            paths_[id.packed()] = *path;
            return;
          }
        }
        // Legacy SOUN: the filename is stored directly.
        const base::String fnam = record.GetString(kFnam);
        if (!fnam.empty()) paths_[id.packed()] = NormalizeSoundPath(fnam);
      });

  RX_INFO("audio: sound catalog built, {} sound forms", paths_.size());
}

base::String SoundCatalog::PathFor(bethesda::GlobalFormId form) const {
  const base::String* path = paths_.find(form.packed());
  return path ? *path : base::String{};
}

}  // namespace rx::audio
