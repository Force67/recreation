#include "bethesda/script_attachment.h"

#include <cstring>

#include "core/log.h"

namespace rec::bethesda {
namespace {

// Little-endian, bounds-checked cursor over a VMAD body.
class Reader {
 public:
  explicit Reader(ByteSpan data) : data_(data) {}
  bool ok() const { return ok_; }
  size_t pos() const { return pos_; }

  u8 U8() {
    if (!Need(1)) return 0;
    return data_[pos_++];
  }
  u16 U16() {
    if (!Need(2)) return 0;
    u16 v = u16(data_[pos_]) | u16(data_[pos_ + 1]) << 8;
    pos_ += 2;
    return v;
  }
  i16 I16() { return static_cast<i16>(U16()); }
  u32 U32() {
    if (!Need(4)) return 0;
    u32 v = u32(data_[pos_]) | u32(data_[pos_ + 1]) << 8 | u32(data_[pos_ + 2]) << 16 |
            u32(data_[pos_ + 3]) << 24;
    pos_ += 4;
    return v;
  }
  i32 I32() { return static_cast<i32>(U32()); }
  f32 F32() {
    u32 v = U32();
    f32 r;
    std::memcpy(&r, &v, 4);
    return r;
  }
  // VMAD strings are uint16 length prefixed, no terminator.
  std::string Str() {
    u16 len = U16();
    if (!Need(len)) return {};
    std::string s(reinterpret_cast<const char*>(data_.data() + pos_), len);
    pos_ += len;
    return s;
  }

 private:
  bool Need(size_t n) {
    if (pos_ + n > data_.size()) {
      ok_ = false;
      return false;
    }
    return ok_;
  }
  ByteSpan data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

ScriptObjectValue ReadObject(Reader& r, i16 object_format) {
  ScriptObjectValue v;
  if (object_format == 1) {
    v.form_id = r.U32();
    v.alias_id = r.U16();
    r.U16();  // unused
  } else {
    r.U16();  // unused
    v.alias_id = r.U16();
    v.form_id = r.U32();
  }
  return v;
}

void ReadPropertyValue(Reader& r, i16 object_format, ScriptProperty* p) {
  switch (p->type) {
    case 1:
      p->object_value = ReadObject(r, object_format);
      break;
    case 2:
      p->string_value = r.Str();
      break;
    case 3:
      p->int_value = r.I32();
      break;
    case 4:
      p->float_value = r.F32();
      break;
    case 5:
      p->bool_value = r.U8() != 0;
      break;
    case 11: {
      u32 n = r.U32();
      for (u32 i = 0; i < n && r.ok(); ++i) p->object_array.push_back(ReadObject(r, object_format));
      break;
    }
    case 12: {
      u32 n = r.U32();
      for (u32 i = 0; i < n && r.ok(); ++i) p->string_array.push_back(r.Str());
      break;
    }
    case 13: {
      u32 n = r.U32();
      for (u32 i = 0; i < n && r.ok(); ++i) p->int_array.push_back(r.I32());
      break;
    }
    case 14: {
      u32 n = r.U32();
      for (u32 i = 0; i < n && r.ok(); ++i) p->float_array.push_back(r.F32());
      break;
    }
    case 15: {
      u32 n = r.U32();
      for (u32 i = 0; i < n && r.ok(); ++i) p->bool_array.push_back(r.U8());
      break;
    }
    default:
      REC_WARN("vmad: unknown property type {}", p->type);
      break;  // cannot size an unknown type; the caller's ok() check fails out
  }
}

// Reads the VMAD header + script list, leaving r positioned at the per-record
// fragment data that may follow. Returns false on a malformed header/scripts.
bool ReadScriptsSection(Reader& r, ScriptAttachment* out) {
  out->version = r.I16();
  out->object_format = r.I16();
  if (out->version < 1 || out->version > 5 ||
      (out->object_format != 1 && out->object_format != 2)) {
    REC_WARN("vmad: bad header version {} format {}", out->version, out->object_format);
    return false;
  }
  bool has_status = out->version >= 4;

  u16 script_count = r.U16();
  out->scripts.reserve(script_count);
  for (u16 i = 0; i < script_count && r.ok(); ++i) {
    ScriptEntry entry;
    entry.name = r.Str();
    if (has_status) entry.status = r.U8();
    u16 prop_count = r.U16();
    entry.properties.reserve(prop_count);
    for (u16 j = 0; j < prop_count && r.ok(); ++j) {
      ScriptProperty prop;
      prop.name = r.Str();
      prop.type = r.U8();
      if (has_status) prop.status = r.U8();
      ReadPropertyValue(r, out->object_format, &prop);
      entry.properties.push_back(std::move(prop));
    }
    out->scripts.push_back(std::move(entry));
  }

  if (!r.ok()) {
    REC_WARN("vmad: truncated near byte {}", r.pos());
    return false;
  }
  return true;
}

}  // namespace

bool ParseScriptAttachment(ByteSpan vmad, ScriptAttachment* out) {
  Reader r(vmad);
  return ReadScriptsSection(r, out);
}

bool ParseQuestFragments(ByteSpan vmad, ScriptAttachment* out,
                         std::vector<QuestStageFragment>* fragments) {
  Reader r(vmad);
  if (!ReadScriptsSection(r, out)) return false;

  // Quest fragment section (SSE): a flags/version byte, the fragment count, the
  // QF script file name, then one entry per stage-with-a-fragment.
  r.U8();                       // flags/version, unused
  u16 fragment_count = r.U16();
  r.Str();                      // shared fragment file name, unused (per-entry name below)
  for (u16 i = 0; i < fragment_count && r.ok(); ++i) {
    QuestStageFragment f;
    f.stage = r.U16();
    r.I16();                    // unknown
    r.I32();                    // log entry / stage index, unused
    r.U8();                     // per-fragment flags, unused
    f.script_name = r.Str();
    f.function = r.Str();
    if (r.ok()) fragments->push_back(std::move(f));
  }
  // Alias fragments may follow; the engine does not need them.
  return true;
}

bool ParseInfoFragments(ByteSpan vmad, ScriptAttachment* out, InfoFragments* frags) {
  Reader r(vmad);
  if (!ReadScriptsSection(r, out)) return false;

  // INFO fragment section (SSE): a version byte, a flags byte (bit 0 = a start
  // fragment is present, bit 1 = an end fragment), the shared TIF file name,
  // then the present fragments, each an unknown byte plus its script and
  // function names.
  r.U8();                  // version, unused
  u8 flags = r.U8();
  r.Str();                 // shared fragment file name, unused
  if (flags & 0x01) {
    r.U8();                // unknown
    frags->begin.script_name = r.Str();
    frags->begin.function = r.Str();
  }
  if (flags & 0x02) {
    r.U8();                // unknown
    frags->end.script_name = r.Str();
    frags->end.function = r.Str();
  }
  if (!r.ok()) {
    frags->begin = InfoFragment{};
    frags->end = InfoFragment{};
  }
  return true;
}

bool ParseSceneFragments(ByteSpan vmad, ScriptAttachment* out, SceneFragments* frags) {
  Reader r(vmad);
  if (!ReadScriptsSection(r, out)) return false;

  // SCEN fragment section (SSE): a version byte, a flags byte (bit 0 = a begin
  // fragment is present, bit 1 = an end fragment), the shared SF_ file name, the
  // present begin/end fragments (each an unknown byte plus its script and
  // function names, like INFO), then a phase count and one entry per phase that
  // has a fragment.
  r.U8();                  // version, unused
  u8 flags = r.U8();
  r.Str();                 // shared fragment file name, unused
  if (flags & 0x01) {
    r.U8();                // unknown
    frags->begin.script_name = r.Str();
    frags->begin.function = r.Str();
  }
  if (flags & 0x02) {
    r.U8();                // unknown
    frags->end.script_name = r.Str();
    frags->end.function = r.Str();
  }
  u16 phase_count = r.U16();
  for (u16 i = 0; i < phase_count && r.ok(); ++i) {
    ScenePhaseFragment p;
    u8 kind = r.U8();      // 1 = a phase-begin fragment, 2 = a phase-end fragment
    p.phase = r.U32();
    r.U8();                // unknown
    p.on_begin = kind != 2;
    p.fragment.script_name = r.Str();
    p.fragment.function = r.Str();
    if (r.ok()) frags->phases.push_back(std::move(p));
  }
  if (!r.ok()) {
    frags->begin = SceneFragment{};
    frags->end = SceneFragment{};
    frags->phases.clear();
  }
  return true;
}

}  // namespace rec::bethesda
