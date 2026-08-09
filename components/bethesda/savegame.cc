#include "components/bethesda/savegame.h"

#include <base/memory/move.h>

#include <cstring>

#include "components/bethesda/compression.h"

namespace rx::bethesda {
namespace {

// What actually separates the supported containers. The rest of this file is
// shared, so another game is a row here instead of another branch on the magic.
struct FormatTraits {
  const char* magic;
  size_t magic_size;
  u32 min_header_version;  // matched top down, so 0 is the catch all
  SaveFormat format;
  u32 screenshot_bpp;
  bool has_codec_field;   // the header ends with a whole body codec selector
  bool has_game_version;  // a wstring sits between formVersion and pluginInfoSize
};

constexpr FormatTraits kFormats[] = {
    {"TESV_SAVEGAME", 13, 12, SaveFormat::kSkyrimSe, 4, true, false},
    {"TESV_SAVEGAME", 13, 0, SaveFormat::kSkyrimLe, 3, false, false},
    {"FO4_SAVEGAME", 12, 0, SaveFormat::kFallout4, 4, false, true},
};

// Whole body codecs as stored in the Skyrim SE header.
constexpr u16 kCodecNone = 0;
constexpr u16 kCodecZlib = 1;

// The global data record that carries the global variable table.
constexpr u32 kGlobalVariablesRecord = 3;
// The one that carries the forms the player made at runtime. It sits in the
// same table as the globals, not in tables 2 or 3.
constexpr u32 kCreatedObjectsRecord = 4;
// Four tables in a fixed order, one per CreatedFormKind.
constexpr u32 kCreatedFormTables = 4;

// Larger than any real save, so a length field past it is corrupt or hostile
// rather than something worth allocating for.
constexpr u64 kMaxBodySize = 512ull * 1024 * 1024;
// DEFLATE cannot expand better than 1032:1, so a ChangeForm claiming more than
// that out of its compressed bytes is lying about one of its two lengths.
constexpr u64 kMaxInflateRatio = 1032;
// Keeps the screenshot dimensions from overflowing their own product. No save
// stores a thumbnail anywhere near this.
constexpr u64 kMaxScreenshotEdge = 1u << 16;
// refid(3) + changeFlags(4) + type(1) + version(1) + the two shortest lengths.
constexpr u64 kMinChangeFormBytes = 11;

// A form id as saves store it: three big endian bytes with a two bit kind in
// the top of the first, which decides how the remaining 22 bits resolve.
struct RefId {
  u8 kind = 0;
  u32 value = 0;
};

// Bounds checked cursor. The first overrun latches `ok_` off and every later
// read returns zero, so a truncated file falls out of the parse rather than
// walking off the buffer. This reads untrusted files.
class Reader {
 public:
  explicit Reader(ByteSpan bytes)
      : begin_(bytes.data()), p_(bytes.data()), end_(bytes.data() + bytes.size()) {}

  bool ok() const { return ok_; }
  size_t remaining() const { return ok_ ? static_cast<size_t>(end_ - p_) : 0; }

  bool Need(size_t n) {
    if (!ok_ || static_cast<size_t>(end_ - p_) < n)
      ok_ = false;
    return ok_;
  }

  u8 U8() {
    if (!Need(1))
      return 0;
    return *p_++;
  }
  u16 U16() {
    if (!Need(2))
      return 0;
    u16 v;
    std::memcpy(&v, p_, 2);
    p_ += 2;
    return v;
  }
  u32 U32() {
    if (!Need(4))
      return 0;
    u32 v;
    std::memcpy(&v, p_, 4);
    p_ += 4;
    return v;
  }
  f32 F32() {
    if (!Need(4))
      return 0.0f;
    f32 v;
    std::memcpy(&v, p_, 4);
    p_ += 4;
    return v;
  }

  ByteSpan Take(size_t n) {
    if (!Need(n))
      return {};
    ByteSpan out(p_, n);
    p_ += n;
    return out;
  }
  bool Skip(size_t n) {
    if (!Need(n))
      return false;
    p_ += n;
    return true;
  }
  bool SeekTo(size_t offset) {
    if (!ok_ || offset > static_cast<size_t>(end_ - begin_)) {
      ok_ = false;
      return false;
    }
    p_ = begin_ + offset;
    return true;
  }

  // A u16 byte count followed by that many bytes. Not nul terminated.
  base::String WString() {
    const u16 len = U16();
    ByteSpan s = Take(len);
    if (!ok_)
      return {};
    return base::String(reinterpret_cast<const char*>(s.data()), s.size());
  }

  // Variable width count: the low two bits give the encoded width, the value
  // is what is left once they are shifted out.
  u32 VsVal() {
    if (!Need(1))
      return 0;
    switch (*p_ & 3u) {
      case 0:
        return U8() >> 2;
      case 1:
        return U16() >> 2;
      case 2:
        return U32() >> 2;
      default:
        ok_ = false;
        return 0;
    }
  }

  RefId ReadRefId() {
    ByteSpan b = Take(3);
    if (!ok_)
      return {};
    RefId r;
    r.kind = static_cast<u8>(b[0] >> 6);
    r.value = (static_cast<u32>(b[0] & 0x3f) << 16) | (static_cast<u32>(b[1]) << 8) | b[2];
    return r;
  }

 private:
  const u8* begin_;
  const u8* p_;
  const u8* end_;
  bool ok_ = true;
};

u32 ResolveRefId(RefId ref, const base::Vector<u32>& form_ids) {
  switch (ref.kind) {
    case 0:  // one based index into the save's own form id array; 0 is none
      if (ref.value == 0 || ref.value > form_ids.size())
        return 0;
      return form_ids[ref.value - 1];
    case 1:  // owned by the first master, so the mod index is already 0
      return ref.value;
    case 2:  // created during play
      return 0xff000000u | ref.value;
    default:  // kind 3 is unused by every save seen; nothing sane to resolve to
      return 0;
  }
}

// The header's play time, the only numeric time it carries. Skyrim writes
// "hours.minutes.seconds" ("527.25.55"); Fallout 4 writes days, hours and
// minutes with a unit letter after each and the whole thing spelled out again
// ("5d.16h.29m.5 days.16 hours.29 minutes"), and both the letters and the words
// are localized, so only the leading three numbers can be read.
f32 PlayTimeSeconds(SaveFormat format, const base::String& text) {
  u32 part[3] = {0, 0, 0};
  size_t i = 0;
  for (u32& value : part) {
    while (i < text.size() && (text[i] < '0' || text[i] > '9'))
      ++i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9')
      value = value * 10 + static_cast<u32>(text[i++] - '0');
  }
  const f32 scale[2][3] = {{3600.0f, 60.0f, 1.0f}, {86400.0f, 3600.0f, 60.0f}};
  const u32 row = format == SaveFormat::kFallout4 ? 1 : 0;
  return static_cast<f32>(part[0]) * scale[row][0] + static_cast<f32>(part[1]) * scale[row][1] +
         static_cast<f32>(part[2]) * scale[row][2];
}

const FormatTraits* MatchFormat(ByteSpan bytes) {
  for (const FormatTraits& t : kFormats) {
    if (bytes.size() < t.magic_size + 8)
      continue;
    if (std::memcmp(bytes.data(), t.magic, t.magic_size) != 0)
      continue;
    u32 version;
    std::memcpy(&version, bytes.data() + t.magic_size + 4, 4);  // past headerSize
    if (version < t.min_header_version)
      continue;
    return &t;
  }
  return nullptr;
}

struct FileLocationTable {
  u32 form_id_array_count_offset = 0;
  u32 unknown_table3_offset = 0;
  u32 global_data_table1_offset = 0;
  u32 global_data_table2_offset = 0;
  u32 change_forms_offset = 0;
  u32 global_data_table3_offset = 0;
  u32 global_data_table1_count = 0;
  u32 global_data_table2_count = 0;
  u32 global_data_table3_count = 0;
  u32 change_form_count = 0;
};

// File location table entries are offsets into the whole file even when the
// body was lifted out of it by decompression. `body_base` is the file offset
// the body logically sits at, which maps one onto the other.
bool BodyOffset(u32 file_offset, size_t body_base, size_t body_size, size_t* out) {
  if (file_offset < body_base)
    return false;
  const size_t offset = file_offset - body_base;
  if (offset > body_size)
    return false;
  *out = offset;
  return true;
}

bool ReadFormIdArray(ByteSpan body, size_t offset, base::Vector<u32>* out) {
  Reader r(body);
  if (!r.SeekTo(offset))
    return false;
  const u32 count = r.U32();
  if (static_cast<u64>(count) * 4 > r.remaining())
    return false;
  ByteSpan raw = r.Take(static_cast<size_t>(count) * 4);
  if (!r.ok())
    return false;
  out->resize(count);
  if (count != 0)
    std::memcpy(out->data(), raw.data(), raw.size());
  return true;
}

bool ReadGlobals(ByteSpan body,
                 size_t offset,
                 u32 record_count,
                 const base::Vector<u32>& form_ids,
                 base::Vector<base::Pair<u32, f32>>* out) {
  Reader r(body);
  if (!r.SeekTo(offset))
    return false;
  // Global data is a flat (type, length, payload) list; only the variable
  // table means anything at this layer, the rest is stepped over by length.
  for (u32 i = 0; i < record_count; ++i) {
    const u32 type = r.U32();
    const u32 length = r.U32();
    ByteSpan payload = r.Take(length);
    if (!r.ok())
      return false;
    if (type != kGlobalVariablesRecord)
      continue;

    Reader v(payload);
    const u32 count = v.VsVal();
    if (static_cast<u64>(count) * 7 > v.remaining())
      return false;
    out->reserve(count);
    for (u32 k = 0; k < count; ++k) {
      const u32 form_id = ResolveRefId(v.ReadRefId(), form_ids);
      out->push_back({form_id, v.F32()});
    }
    if (!v.ok())
      return false;
  }
  return true;
}

// Created objects, walked out of the same table the globals live in. Four
// vsval-counted arrays back to back, each entry a RefID, a count, and a
// vsval-counted list of magic effects. The whole record is consumed exactly,
// which is what says the layout is right.
bool ReadCreatedObjects(ByteSpan body,
                        size_t offset,
                        u32 record_count,
                        const base::Vector<u32>& form_ids,
                        base::Vector<CreatedForm>* out) {
  Reader r(body);
  if (!r.SeekTo(offset))
    return false;
  for (u32 i = 0; i < record_count; ++i) {
    const u32 type = r.U32();
    const u32 length = r.U32();
    ByteSpan payload = r.Take(length);
    if (!r.ok())
      return false;
    if (type != kCreatedObjectsRecord)
      continue;

    Reader c(payload);
    for (u32 table = 0; table < kCreatedFormTables; ++table) {
      const u32 count = c.VsVal();
      // Smallest entry is RefID(3) + count(4) + an empty effect list(1).
      if (!c.ok() || static_cast<u64>(count) * 8 > c.remaining())
        return false;
      for (u32 k = 0; k < count && c.ok(); ++k) {
        CreatedForm form;
        form.form_id = ResolveRefId(c.ReadRefId(), form_ids);
        form.unknown_count = c.U32();
        form.kind = static_cast<CreatedFormKind>(table);
        const u32 effects = c.VsVal();
        if (!c.ok() || static_cast<u64>(effects) * 19 > c.remaining())
          return false;
        for (u32 e = 0; e < effects; ++e) {
          CreatedEffect effect;
          effect.effect = ResolveRefId(c.ReadRefId(), form_ids);
          effect.magnitude = c.F32();
          effect.duration = c.U32();
          c.U32();  // area; zero for every effect in the reference save
          effect.value = c.F32();
          form.effects.push_back(effect);
        }
        out->push_back(base::move(form));
      }
    }
    if (!c.ok())
      return false;
  }
  return true;
}

bool ReadChangeForms(ByteSpan body,
                     size_t offset,
                     u32 count,
                     SaveFormat format,
                     const base::Vector<u32>& form_ids,
                     base::Vector<ChangeForm>* out) {
  Reader r(body);
  if (!r.SeekTo(offset))
    return false;
  // Every record is at least this big, so a count that cannot fit in what is
  // left is a corrupt table and not something to reserve for.
  if (static_cast<u64>(count) * kMinChangeFormBytes > r.remaining())
    return false;
  out->reserve(count);

  for (u32 i = 0; i < count; ++i) {
    const RefId ref = r.ReadRefId();
    const u32 flags = r.U32();
    const u8 type_byte = r.U8();
    const u8 version = r.U8();

    // The top two bits of the type byte size the two lengths that follow it.
    u32 length1 = 0;
    u32 length2 = 0;
    switch (type_byte >> 6) {
      case 0:
        length1 = r.U8();
        length2 = r.U8();
        break;
      case 1:
        length1 = r.U16();
        length2 = r.U16();
        break;
      case 2:
        length1 = r.U32();
        length2 = r.U32();
        break;
      default:
        return false;
    }
    ByteSpan payload = r.Take(length1);
    if (!r.ok())
      return false;

    ChangeForm& form = out->emplace_back();
    form.form_id = ResolveRefId(ref, form_ids);
    form.type = ChangeFormTypeOf(format, type_byte);
    form.flags = flags;
    form.version = version;

    // A non zero length2 means the payload is zlib compressed to length1 bytes
    // and inflates to length2. Skyrim SE does this per record even though the
    // whole body around it is LZ4.
    if (length2 == 0) {
      form.data.resize(length1);
      if (length1 != 0)
        std::memcpy(form.data.data(), payload.data(), length1);
      continue;
    }
    // The ratio bound alone still lets a 30 MB record claim it inflates to 4 GB,
    // so cap it at the body it came out of as well.
    if (length2 > kMaxInflateRatio * length1 || length2 > body.size())
      return false;
    form.data.resize(length2);
    if (!ZlibInflate(payload, form.data.data(), length2))
      return false;
  }
  return r.ok();
}

// A compressed body is prefixed by its two sizes. Those two u32 sit at the
// file offset the body is measured from, but are not themselves part of it.
bool ReadBody(Reader& r,
              const FormatTraits& traits,
              u16 codec,
              base::Vector<u8>* storage,
              ByteSpan* body) {
  if (!traits.has_codec_field || codec == kCodecNone) {
    *body = r.Take(r.remaining());
    return r.ok();
  }

  const u32 decompressed_size = r.U32();
  const u32 compressed_size = r.U32();
  ByteSpan src = r.Take(compressed_size);
  if (!r.ok() || compressed_size == 0 || decompressed_size == 0 ||
      decompressed_size > kMaxBodySize)
    return false;

  storage->resize(decompressed_size);
  const bool decoded = codec == kCodecZlib
                           ? ZlibInflate(src, storage->data(), decompressed_size)
                           : Lz4BlockDecompress(src, storage->data(), decompressed_size);
  if (!decoded)
    return false;
  *body = ByteSpan(storage->data(), storage->size());
  return true;
}

}  // namespace

SaveFormat DetectSaveFormat(ByteSpan bytes) {
  const FormatTraits* traits = MatchFormat(bytes);
  return traits ? traits->format : SaveFormat::kUnknown;
}

ChangeFormType ChangeFormTypeOf(SaveFormat format, u8 type_byte) {
  const u8 value = type_byte & 0x3f;
  // The first type Fallout 4 does not have is INGR, so from its slot up the
  // enumeration is Skyrim's shifted down by one (see the header).
  if (format == SaveFormat::kFallout4 && value >= static_cast<u8>(ChangeFormType::kIngr))
    return static_cast<ChangeFormType>(value + 1);
  return static_cast<ChangeFormType>(value);
}

bool ReadSaveFile(ByteSpan bytes, SaveFile& out) {
  const FormatTraits* traits = MatchFormat(bytes);
  if (!traits)
    return false;

  SaveFile save;
  save.format = traits->format;

  Reader r(bytes);
  r.Skip(traits->magic_size);
  const u32 header_size = r.U32();
  const size_t header_start = traits->magic_size + 4;

  r.U32();  // header version, already read by MatchFormat
  save.save_number = r.U32();
  save.player_name = r.WString();
  save.player_level = r.U32();
  save.player_location = r.WString();
  save.game_time = r.WString();
  r.WString();  // player race editor id
  r.U16();      // player sex
  r.F32();      // current experience
  r.F32();      // experience needed for the next level
  r.Skip(8);    // FILETIME the save was written at
  const u32 shot_width = r.U32();
  const u32 shot_height = r.U32();
  const u16 codec = traits->has_codec_field ? r.U16() : kCodecNone;
  if (!r.ok())
    return false;
  save.in_game_seconds = PlayTimeSeconds(save.format, save.game_time);

  // Trust the header's own size over the field walk above, so a version that
  // appended a field still lands on the screenshot.
  if (!r.SeekTo(header_start + header_size))
    return false;
  if (shot_width > kMaxScreenshotEdge || shot_height > kMaxScreenshotEdge)
    return false;
  const u64 screenshot_size =
      static_cast<u64>(shot_width) * shot_height * traits->screenshot_bpp;
  if (screenshot_size > r.remaining() || !r.Skip(static_cast<size_t>(screenshot_size)))
    return false;

  const size_t body_base = header_start + header_size + static_cast<size_t>(screenshot_size);
  base::Vector<u8> storage;
  ByteSpan body;
  if (!ReadBody(r, *traits, codec, &storage, &body))
    return false;

  Reader b(body);
  b.U8();  // form version
  if (traits->has_game_version)
    b.WString();

  const u32 plugin_info_size = b.U32();
  ByteSpan plugin_info = b.Take(plugin_info_size);
  if (!b.ok())
    return false;
  {
    Reader p(plugin_info);
    const u32 plugin_count = p.U8();
    save.plugins.reserve(plugin_count);
    for (u32 i = 0; i < plugin_count && p.ok(); ++i)
      save.plugins.push_back(p.WString());
    // Skyrim SE appends the light plugins inside the same sized block, so the
    // bytes left over after the regular list are what says they are there.
    if (p.ok() && p.remaining() >= 2) {
      const u32 light_count = p.U16();
      save.light_plugins.reserve(light_count);
      for (u32 i = 0; i < light_count && p.ok(); ++i)
        save.light_plugins.push_back(p.WString());
    }
    if (!p.ok())
      return false;
  }

  FileLocationTable flt;
  flt.form_id_array_count_offset = b.U32();
  flt.unknown_table3_offset = b.U32();
  flt.global_data_table1_offset = b.U32();
  flt.global_data_table2_offset = b.U32();
  flt.change_forms_offset = b.U32();
  flt.global_data_table3_offset = b.U32();
  flt.global_data_table1_count = b.U32();
  flt.global_data_table2_count = b.U32();
  flt.global_data_table3_count = b.U32();
  flt.change_form_count = b.U32();
  if (!b.ok())
    return false;

  size_t form_id_offset = 0;
  size_t globals_offset = 0;
  size_t change_forms_offset = 0;
  if (!BodyOffset(flt.form_id_array_count_offset, body_base, body.size(), &form_id_offset) ||
      !BodyOffset(flt.global_data_table1_offset, body_base, body.size(), &globals_offset) ||
      !BodyOffset(flt.change_forms_offset, body_base, body.size(), &change_forms_offset))
    return false;

  // The form id map has to come first: everything below resolves RefIDs
  // through it, and it is stored after the records that reference it.
  if (!ReadFormIdArray(body, form_id_offset, &save.form_ids))
    return false;
  if (!ReadGlobals(body, globals_offset, flt.global_data_table1_count, save.form_ids,
                   &save.globals))
    return false;
  if (!ReadCreatedObjects(body, globals_offset, flt.global_data_table1_count, save.form_ids,
                          &save.created_forms))
    return false;
  if (!ReadChangeForms(body, change_forms_offset, flt.change_form_count, save.format,
                       save.form_ids, &save.change_forms))
    return false;

  out = base::move(save);
  return true;
}

}  // namespace rx::bethesda
