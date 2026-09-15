#include "components/bethesda/load_screen.h"

#include <cstring>

#include "core/log.h"

namespace rx::bethesda {

namespace {

constexpr u32 kLscr = FourCc('L', 'S', 'C', 'R');
constexpr u32 kNnam = FourCc('N', 'N', 'A', 'M');
constexpr u32 kDesc = FourCc('D', 'E', 'S', 'C');
constexpr u32 kSnam = FourCc('S', 'N', 'A', 'M');
constexpr u32 kRnam = FourCc('R', 'N', 'A', 'M');
constexpr u32 kXnam = FourCc('X', 'N', 'A', 'M');
constexpr u32 kModl = FourCc('M', 'O', 'D', 'L');

template <typename T>
bool ReadAt(const Subrecord* sub, size_t offset, T* out) {
  if (!sub || sub->data.size() < offset + sizeof(T))
    return false;
  std::memcpy(out, sub->data.data() + offset, sizeof(T));
  return true;
}

}  // namespace

int LoadLoadScreens(const RecordStore& records, base::Vector<LoadScreen>* out) {
  if (!out)
    return 0;
  records.EachOfType(kLscr, [&](GlobalFormId id, const RecordStore::StoredRecord& stored) {
    Record record;
    if (!records.Parse(id, &record))
      return;
    const Subrecord* nnam = record.Find(kNnam);
    u32 raw_model = 0;
    if (!ReadAt(nnam, 0, &raw_model) || raw_model == 0)
      return;  // no model: nothing to show, so not a screen we can use
    LoadScreen screen;
    screen.id = id;
    screen.model = records.ResolveFrom(RawFormId{raw_model}, stored.winning_plugin);
    if (screen.model.plugin == 0xffff)
      return;
    ReadAt(record.Find(kDesc), 0, &screen.description);
    if (!ReadAt(record.Find(kSnam), 0, &screen.scale) || screen.scale <= 0.0f)
      screen.scale = 1.0f;
    // RNAM is three signed degrees, packed back to back.
    if (const Subrecord* rnam = record.Find(kRnam)) {
      for (int axis = 0; axis < 3; ++axis)
        ReadAt(rnam, static_cast<size_t>(axis) * 2, &screen.rotation[axis]);
    }
    if (const Subrecord* xnam = record.Find(kXnam)) {
      for (int axis = 0; axis < 3; ++axis)
        ReadAt(xnam, static_cast<size_t>(axis) * 4, &screen.offset[axis]);
    }
    out->push_back(screen);
  });
  return static_cast<int>(out->size());
}

base::String LoadScreenModelPath(const RecordStore& records, const LoadScreen& screen) {
  if (screen.model.plugin == 0xffff)
    return {};
  Record record;
  if (!records.Parse(screen.model, &record))
    return {};
  const Subrecord* modl = record.Find(kModl);
  if (!modl || modl->data.empty())
    return {};
  base::String path(reinterpret_cast<const char*>(modl->data.data()), modl->data.size());
  if (const mem_size zero = path.find('\0'); zero != base::String::npos)
    path.resize(zero);
  if (path.empty())
    return {};
  for (char& c : path) {
    if (c == '\\')
      c = '/';
    else
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  // MODL is authored relative to the meshes directory; the asset database wants
  // the full virtual path.
  if (!path.starts_with("meshes/"))
    path = "meshes/" + path;
  return path;
}

}  // namespace rx::bethesda
