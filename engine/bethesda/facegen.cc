#include "bethesda/facegen.h"

#include <cstring>

#include "bethesda/load_order.h"
#include "bethesda/record.h"

namespace rec::bethesda {
namespace {

constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kModl = FourCc('M', 'O', 'D', 'L');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kPnam = FourCc('P', 'N', 'A', 'M');
constexpr u32 kHnam = FourCc('H', 'N', 'A', 'M');
constexpr u32 kNam0 = FourCc('N', 'A', 'M', '0');
constexpr u32 kNam1 = FourCc('N', 'A', 'M', '1');
constexpr u32 kTnam = FourCc('T', 'N', 'A', 'M');
constexpr u32 kCnam = FourCc('C', 'N', 'A', 'M');
constexpr u32 kRnam = FourCc('R', 'N', 'A', 'M');
constexpr u32 kFnam = FourCc('F', 'N', 'A', 'M');
constexpr u32 kHclf = FourCc('H', 'C', 'L', 'F');
constexpr u32 kFtst = FourCc('F', 'T', 'S', 'T');
constexpr u32 kQnam = FourCc('Q', 'N', 'A', 'M');
constexpr u32 kNam9 = FourCc('N', 'A', 'M', '9');
constexpr u32 kNama = FourCc('N', 'A', 'M', 'A');
constexpr u32 kTini = FourCc('T', 'I', 'N', 'I');
constexpr u32 kTinc = FourCc('T', 'I', 'N', 'C');
constexpr u32 kTinv = FourCc('T', 'I', 'N', 'V');
constexpr u32 kTias = FourCc('T', 'I', 'A', 'S');
constexpr u32 kMnam = FourCc('M', 'N', 'A', 'M');
constexpr u32 kIndx = FourCc('I', 'N', 'D', 'X');
constexpr u32 kHead = FourCc('H', 'E', 'A', 'D');
constexpr u32 kMpai = FourCc('M', 'P', 'A', 'I');
constexpr u32 kMpav = FourCc('M', 'P', 'A', 'V');
constexpr u32 kRprm = FourCc('R', 'P', 'R', 'M');
constexpr u32 kAhcm = FourCc('A', 'H', 'C', 'M');
constexpr u32 kFtsm = FourCc('F', 'T', 'S', 'M');
constexpr u32 kDftm = FourCc('D', 'F', 'T', 'M');
constexpr u32 kTint = FourCc('T', 'I', 'N', 'T');
constexpr u32 kTinp = FourCc('T', 'I', 'N', 'P');
constexpr u32 kTind = FourCc('T', 'I', 'N', 'D');

std::string SubString(const Subrecord& sub) {
  if (sub.data.empty()) return {};
  size_t len = sub.data.size();
  if (sub.data[len - 1] == 0) --len;
  return std::string(reinterpret_cast<const char*>(sub.data.data()), len);
}

template <typename T>
T ReadAt(const Subrecord& sub, size_t offset = 0) {
  T value{};
  if (offset + sizeof(T) <= sub.data.size()) std::memcpy(&value, sub.data.data() + offset, sizeof(T));
  return value;
}

GlobalFormId ReadFormRef(const RecordStore& store, const Subrecord* sub, u16 plugin) {
  if (!sub || sub->data.size() < 4) return {};
  u32 raw = ReadAt<u32>(*sub);
  if (raw == 0) return {};
  return store.ResolveFrom(RawFormId{raw}, plugin);
}

}  // namespace

std::optional<HeadPart> ResolveHeadPart(const RecordStore& store, GlobalFormId id) {
  const RecordStore::StoredRecord* stored = store.Find(id);
  if (!stored || stored->header.type != FourCc('H', 'D', 'P', 'T')) return std::nullopt;
  Record rec;
  if (!store.Parse(id, &rec)) return std::nullopt;
  const u16 plugin = stored->winning_plugin;

  HeadPart part;
  part.id = id;
  part.editor_id = rec.GetString(kEdid);
  part.model = rec.GetString(kModl);
  if (const Subrecord* pnam = rec.Find(kPnam); pnam && pnam->data.size() >= 4)
    part.type = static_cast<HeadPartType>(ReadAt<u32>(*pnam));
  if (const Subrecord* data = rec.Find(kData); data && !data->data.empty())
    part.flags = data->data[0];
  part.texture_set = ReadFormRef(store, rec.Find(kTnam), plugin);
  part.color = ReadFormRef(store, rec.Find(kCnam), plugin);
  part.valid_races = ReadFormRef(store, rec.Find(kRnam), plugin);

  // HNAM extra parts and NAM0/NAM1 tri pairs come in subrecord order.
  u32 pending_type = 0;
  for (const Subrecord& sub : rec.subrecords) {
    if (sub.type == kHnam && sub.data.size() >= 4) {
      part.extra_parts.push_back(store.ResolveFrom(RawFormId{ReadAt<u32>(sub)}, plugin));
    } else if (sub.type == kNam0 && sub.data.size() >= 4) {
      pending_type = ReadAt<u32>(sub);
    } else if (sub.type == kNam1) {
      part.tris.push_back({pending_type, SubString(sub)});
      pending_type = 0;
    }
  }
  return part;
}

std::optional<ColorForm> ResolveColorForm(const RecordStore& store, GlobalFormId id) {
  const RecordStore::StoredRecord* stored = store.Find(id);
  if (!stored || stored->header.type != FourCc('C', 'L', 'F', 'M')) return std::nullopt;
  Record rec;
  if (!store.Parse(id, &rec)) return std::nullopt;
  ColorForm color;
  color.id = id;
  color.editor_id = rec.GetString(kEdid);
  if (const Subrecord* cnam = rec.Find(kCnam); cnam && cnam->data.size() >= 4)
    std::memcpy(color.rgba, cnam->data.data(), 4);
  if (const Subrecord* fnam = rec.Find(kFnam); fnam && fnam->data.size() >= 4)
    color.playable = ReadAt<u32>(*fnam) != 0;
  return color;
}

std::optional<NpcFaceData> ResolveNpcFace(const RecordStore& store, GlobalFormId id) {
  const RecordStore::StoredRecord* stored = store.Find(id);
  if (!stored || stored->header.type != FourCc('N', 'P', 'C', '_')) return std::nullopt;
  Record rec;
  if (!store.Parse(id, &rec)) return std::nullopt;
  const u16 plugin = stored->winning_plugin;

  NpcFaceData face;
  face.id = id;
  face.editor_id = rec.GetString(kEdid);
  face.race = ReadFormRef(store, rec.Find(kRnam), plugin);
  face.hair_color = ReadFormRef(store, rec.Find(kHclf), plugin);
  face.face_texture_set = ReadFormRef(store, rec.Find(kFtst), plugin);

  if (const Subrecord* qnam = rec.Find(kQnam); qnam && qnam->data.size() >= 12) {
    std::memcpy(face.skin_tone, qnam->data.data(), 12);
    face.has_skin_tone = true;
  }
  if (const Subrecord* nam9 = rec.Find(kNam9); nam9 && nam9->data.size() >= kFaceMorphCount * 4) {
    std::memcpy(face.face_morph, nam9->data.data(), kFaceMorphCount * 4);
    face.has_face_morph = true;
  }
  if (const Subrecord* nama = rec.Find(kNama); nama && nama->data.size() >= 16) {
    std::memcpy(face.face_parts, nama->data.data(), 16);
    face.has_face_parts = true;
  }

  // PNAM head parts and TINI/TINC/TINV/TIAS tint groups, both in file order. A
  // tint layer opens on TINI; its color/alpha/preset follow.
  for (const Subrecord& sub : rec.subrecords) {
    if (sub.type == kPnam && sub.data.size() >= 4) {
      u32 raw = ReadAt<u32>(sub);
      if (raw != 0) face.head_parts.push_back(store.ResolveFrom(RawFormId{raw}, plugin));
    } else if (sub.type == kTini) {
      NpcTintLayer layer;
      layer.index = ReadAt<u16>(sub);
      face.tint_layers.push_back(layer);
    } else if (sub.type == kTinc && !face.tint_layers.empty()) {
      if (sub.data.size() >= 4) std::memcpy(face.tint_layers.back().color, sub.data.data(), 4);
    } else if (sub.type == kTinv && !face.tint_layers.empty()) {
      face.tint_layers.back().interpolation = ReadAt<u32>(sub);
    } else if (sub.type == kTias && !face.tint_layers.empty()) {
      face.tint_layers.back().preset = ReadAt<i16>(sub);
    }
  }
  return face;
}

std::optional<RaceHeadData> ResolveRaceHead(const RecordStore& store, GlobalFormId id) {
  const RecordStore::StoredRecord* stored = store.Find(id);
  if (!stored || stored->header.type != FourCc('R', 'A', 'C', 'E')) return std::nullopt;
  Record rec;
  if (!store.Parse(id, &rec)) return std::nullopt;
  const u16 plugin = stored->winning_plugin;

  RaceHeadData race;
  race.id = id;
  race.editor_id = rec.GetString(kEdid);

  // The head-data section starts at the NAM0 marker; before it MNAM/FNAM mark
  // the body/skeleton blocks and must be ignored. Inside, MNAM opens the male
  // block and FNAM the female one. Everything is a flat subrecord stream, so
  // walk it and route into the active sex block.
  bool in_head = false;
  RaceSexHead* sex = nullptr;
  for (const Subrecord& sub : rec.subrecords) {
    if (sub.type == kNam0) {
      in_head = true;
      continue;
    }
    if (!in_head) continue;
    if (sub.type == kMnam) {
      sex = &race.male;
    } else if (sub.type == kFnam) {
      sex = &race.female;
    } else if (!sex) {
      continue;
    } else if (sub.type == kIndx && sub.data.size() >= 4) {
      RaceHeadPart p;
      p.index = ReadAt<u32>(sub);
      sex->parts.push_back(p);
    } else if (sub.type == kHead && sub.data.size() >= 4) {
      if (!sex->parts.empty())
        sex->parts.back().head_part = store.ResolveFrom(RawFormId{ReadAt<u32>(sub)}, plugin);
    } else if (sub.type == kMpai && sub.data.size() >= 4) {
      RaceMorphAvail avail;
      avail.index = ReadAt<u32>(sub);
      sex->morph_avail.push_back(avail);
    } else if (sub.type == kMpav && !sex->morph_avail.empty()) {
      size_t n = sub.data.size() < 32 ? sub.data.size() : 32;
      std::memcpy(sex->morph_avail.back().mask, sub.data.data(), n);
    } else if (sub.type == kRprm) {
      sex->presets.push_back(ReadFormRef(store, &sub, plugin));
    } else if (sub.type == kAhcm) {
      sex->hair_colors.push_back(ReadFormRef(store, &sub, plugin));
    } else if (sub.type == kFtsm) {
      sex->face_texture_sets.push_back(ReadFormRef(store, &sub, plugin));
    } else if (sub.type == kDftm) {
      sex->default_face_texture_set = ReadFormRef(store, &sub, plugin);
    } else if (sub.type == kTini) {
      RaceTintLayer layer;
      layer.index = ReadAt<u16>(sub);
      sex->tint_layers.push_back(layer);
    } else if (sub.type == kTint && !sex->tint_layers.empty()) {
      sex->tint_layers.back().mask_texture = SubString(sub);
    } else if (sub.type == kTinp && !sex->tint_layers.empty()) {
      sex->tint_layers.back().mask_type = ReadAt<u16>(sub);
    } else if (sub.type == kTind && !sex->tint_layers.empty()) {
      sex->tint_layers.back().default_color = ReadFormRef(store, &sub, plugin);
    } else if (sub.type == kTinc && !sex->tint_layers.empty()) {
      // In RACE, TINC is a CLFM ref that opens a preset entry finished by TINV.
      RaceTintPreset preset;
      preset.color = ReadFormRef(store, &sub, plugin);
      sex->tint_layers.back().presets.push_back(preset);
    } else if (sub.type == kTinv && !sex->tint_layers.empty() &&
               !sex->tint_layers.back().presets.empty()) {
      sex->tint_layers.back().presets.back().weight = ReadAt<f32>(sub);
    }
  }
  return race;
}

const char* FaceMorphName(u32 index) {
  // NAM9 slider order verified as 19 floats; semantics per UESP/CK face-part
  // advanced tab. The trailing slot has no documented label.
  static const char* kNames[kFaceMorphCount] = {
      "NoseLong",   "NoseUp",    "JawUp",       "JawWide",   "JawForward",
      "CheeksUp",   "CheeksBack", "EyesUp",     "EyesIn",    "BrowsUp",
      "BrowsIn",    "BrowsBack", "LipsUp",      "LipsOut",   "ChinWide",
      "ChinUp",     "ChinUnderbite", "EyesForward", "Unknown"};
  return index < kFaceMorphCount ? kNames[index] : "?";
}

}  // namespace rec::bethesda
