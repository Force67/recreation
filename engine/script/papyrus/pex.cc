#include "script/papyrus/pex.h"

#include <cstring>

#include "core/log.h"

namespace rx::script::papyrus {
namespace {

// Bounds-checked, endian-aware cursor over the .pex blob. Any read past the
// end flips ok() and returns zero, so callers can decode straight through and
// check ok() once at the end instead of after every field.
class Reader {
 public:
  Reader(ByteSpan data, bool big_endian) : data_(data), big_(big_endian) {}

  bool ok() const { return ok_; }
  size_t pos() const { return pos_; }

  u8 U8() {
    if (!Need(1)) return 0;
    return data_[pos_++];
  }
  u16 U16() {
    if (!Need(2)) return 0;
    const u8* p = data_.data() + pos_;
    pos_ += 2;
    return big_ ? u16(p[0]) << 8 | p[1] : u16(p[1]) << 8 | p[0];
  }
  u32 U32() {
    if (!Need(4)) return 0;
    const u8* p = data_.data() + pos_;
    pos_ += 4;
    if (big_) return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3];
    return u32(p[3]) << 24 | u32(p[2]) << 16 | u32(p[1]) << 8 | p[0];
  }
  u64 U64() {
    u64 a = U32(), b = U32();
    return big_ ? a << 32 | b : b << 32 | a;
  }
  i32 I32() {
    u32 v = U32();
    i32 r;
    std::memcpy(&r, &v, 4);
    return r;
  }
  f32 F32() {
    u32 v = U32();
    f32 r;
    std::memcpy(&r, &v, 4);
    return r;
  }
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
  bool big_;
  bool ok_ = true;
};

VariableData ReadVarData(Reader& r) {
  VariableData v;
  v.type = static_cast<VariableData::Type>(r.U8());
  switch (v.type) {
    case VariableData::Type::kNone:
      break;
    case VariableData::Type::kIdentifier:
    case VariableData::Type::kString:
      v.string_index = r.U16();
      break;
    case VariableData::Type::kInteger:
      v.int_value = r.I32();
      break;
    case VariableData::Type::kFloat:
      v.float_value = r.F32();
      break;
    case VariableData::Type::kBool:
      v.bool_value = r.U8() != 0;
      break;
    default:
      RX_WARN("pex: bad operand type {} at byte {}", static_cast<int>(v.type), r.pos());
      // Force failure: an unknown operand type means the stream is desynced.
      r.U8();  // keep advancing so pos() is meaningful, ok() set below by caller
      v.type = VariableData::Type::kNone;
      break;
  }
  return v;
}

Function ReadFunction(Reader& r) {
  Function f;
  f.return_type = r.U16();
  f.doc_string = r.U16();
  f.user_flags = r.U32();
  u8 flags = r.U8();
  f.is_global = (flags & 0x1) != 0;
  f.is_native = (flags & 0x2) != 0;

  u16 param_count = r.U16();
  f.params.reserve(param_count);
  for (u16 i = 0; i < param_count; ++i) f.params.push_back({r.U16(), r.U16()});

  u16 local_count = r.U16();
  f.locals.reserve(local_count);
  for (u16 i = 0; i < local_count; ++i) f.locals.push_back({r.U16(), r.U16()});

  u16 instr_count = r.U16();
  f.code.reserve(instr_count);
  for (u16 i = 0; i < instr_count && r.ok(); ++i) {
    Instruction insn;
    u8 raw = r.U8();
    insn.op = static_cast<Op>(raw);
    const OpInfo& info = GetOpInfo(raw);
    if (std::strcmp(info.mnemonic, "invalid") == 0) {
      RX_WARN("pex: unknown opcode 0x{:02x} at byte {}", raw, r.pos());
      return f;  // caller sees !ok via the truncation check, or count mismatch
    }
    for (u8 a = 0; a < info.fixed_args; ++a) insn.args.push_back(ReadVarData(r));
    if (info.var_args) {
      VariableData count = ReadVarData(r);
      i32 n = count.int_value;
      for (i32 a = 0; a < n && r.ok(); ++a) insn.var_args.push_back(ReadVarData(r));
    }
    f.code.push_back(std::move(insn));
  }
  return f;
}

// `fallout` selects the Fallout 4/76 object layout, which adds three fields the
// Skyrim layout lacks (verified against shipped FO4 .pex via the per-object size
// field): an object const-flag byte, a struct-definition section after the auto
// state, and a const-flag byte on each member variable. Getting any of these
// wrong shifts the function-code offset and desyncs the instruction decoder.
Object ReadObject(Reader& r, bool fallout) {
  Object o;
  o.name = r.U16();
  r.U32();  // object data size in bytes; we parse sequentially and ignore it
  o.parent_class = r.U16();
  o.doc_string = r.U16();
  if (fallout) r.U8();  // object const flag (unused by the VM)
  o.user_flags = r.U32();
  o.auto_state_name = r.U16();

  if (fallout) {
    // Struct definitions, between the auto state and the variables. The VM does
    // not model struct types yet, so they are skipped to keep the cursor aligned.
    u16 struct_count = r.U16();
    for (u16 i = 0; i < struct_count; ++i) {
      r.U16();  // struct name
      u16 member_count = r.U16();
      for (u16 j = 0; j < member_count; ++j) {
        r.U16();         // member name
        r.U16();         // member type
        r.U32();         // member user flags
        ReadVarData(r);  // member default value
        r.U8();          // member const flag
        r.U16();         // member doc string
      }
    }
  }

  u16 var_count = r.U16();
  o.variables.reserve(var_count);
  for (u16 i = 0; i < var_count; ++i) {
    MemberVariable v;
    v.name = r.U16();
    v.type = r.U16();
    v.user_flags = r.U32();
    v.initial_value = ReadVarData(r);
    if (fallout) r.U8();  // member variable const flag
    o.variables.push_back(std::move(v));
  }

  u16 prop_count = r.U16();
  o.properties.reserve(prop_count);
  for (u16 i = 0; i < prop_count; ++i) {
    Property p;
    p.name = r.U16();
    p.type = r.U16();
    p.doc_string = r.U16();
    p.user_flags = r.U32();
    p.flags = r.U8();
    if (p.is_auto()) {
      p.auto_var_name = r.U16();
    } else {
      if (p.readable()) {
        p.getter = ReadFunction(r);
        p.has_getter = true;
      }
      if (p.writable()) {
        p.setter = ReadFunction(r);
        p.has_setter = true;
      }
    }
    o.properties.push_back(std::move(p));
  }

  u16 state_count = r.U16();
  o.states.reserve(state_count);
  for (u16 i = 0; i < state_count; ++i) {
    State s;
    s.name = r.U16();
    u16 fn_count = r.U16();
    s.functions.reserve(fn_count);
    for (u16 j = 0; j < fn_count; ++j) {
      NamedFunction nf;
      nf.name = r.U16();
      nf.function = ReadFunction(r);
      s.functions.push_back(std::move(nf));
    }
    o.states.push_back(std::move(s));
  }
  return o;
}

// Fallout .pex carries two extra debug sections (property groups, struct member
// order) after the function table. Skyrim does not. Consumed here only to keep
// the cursor aligned for the user-flag and object tables that follow.
void SkipFalloutDebugExtras(Reader& r) {
  u16 group_count = r.U16();
  for (u16 i = 0; i < group_count; ++i) {
    r.U16();  // object name
    r.U16();  // group name
    r.U16();  // doc string
    r.U32();  // user flags
    u16 names = r.U16();
    for (u16 j = 0; j < names; ++j) r.U16();
  }
  u16 struct_count = r.U16();
  for (u16 i = 0; i < struct_count; ++i) {
    r.U16();  // object name
    r.U16();  // struct name
    u16 names = r.U16();
    for (u16 j = 0; j < names; ++j) r.U16();
  }
}

const std::string kEmpty;

}  // namespace

const std::string& PexFile::Str(StringIndex index) const {
  if (index >= string_table.size()) return kEmpty;
  return string_table[index];
}

bool ParsePex(ByteSpan data, PexFile* out) {
  if (data.size() < 4) {
    RX_WARN("pex: too small ({} bytes)", data.size());
    return false;
  }
  bool big;
  if (data[0] == 0xFA && data[1] == 0x57 && data[2] == 0xC0 && data[3] == 0xDE) {
    big = true;  // Skyrim LE/SE
  } else if (data[0] == 0xDE && data[1] == 0xC0 && data[2] == 0x57 && data[3] == 0xFA) {
    big = false;  // Fallout 4/76
  } else {
    RX_WARN("pex: bad magic {:02x} {:02x} {:02x} {:02x}", data[0], data[1], data[2], data[3]);
    return false;
  }

  Reader r(data, big);
  r.U32();  // magic, already validated
  out->big_endian = big;
  out->major_version = r.U8();
  out->minor_version = r.U8();
  out->game_id = r.U16();
  out->compilation_time = r.U64();
  out->source_file_name = r.Str();
  out->user_name = r.Str();
  out->machine_name = r.Str();

  u16 string_count = r.U16();
  out->string_table.reserve(string_count);
  for (u16 i = 0; i < string_count; ++i) out->string_table.push_back(r.Str());

  out->has_debug_info = r.U8() != 0;
  if (out->has_debug_info) {
    out->modification_time = r.U64();
    u16 fn_count = r.U16();
    out->debug_functions.reserve(fn_count);
    for (u16 i = 0; i < fn_count; ++i) {
      DebugFunction d;
      d.object_name = r.U16();
      d.state_name = r.U16();
      d.function_name = r.U16();
      d.function_type = r.U8();
      u16 line_count = r.U16();
      d.line_numbers.reserve(line_count);
      for (u16 j = 0; j < line_count; ++j) d.line_numbers.push_back(r.U16());
      out->debug_functions.push_back(std::move(d));
    }
    if (out->game_id != 1) SkipFalloutDebugExtras(r);
  }

  u16 user_flag_count = r.U16();
  out->user_flags.reserve(user_flag_count);
  for (u16 i = 0; i < user_flag_count; ++i) {
    PexFile::UserFlag f;
    f.name = r.U16();
    f.bit_index = r.U8();
    out->user_flags.push_back(f);
  }

  u16 object_count = r.U16();
  out->objects.reserve(object_count);
  const bool fallout = out->game_id != 1;  // Skyrim is 1; FO4/76 add object fields
  for (u16 i = 0; i < object_count && r.ok(); ++i)
    out->objects.push_back(ReadObject(r, fallout));

  if (!r.ok()) {
    RX_WARN("pex: truncated or malformed near byte {}", r.pos());
    return false;
  }
  return true;
}

}  // namespace rx::script::papyrus
