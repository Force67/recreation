#include "components/swf/abc.h"

#include <base/memory/move.h>
#include <base/strings/format.h>

#include <cstring>

namespace rx::swf {
namespace {

// A guard against a corrupt count field turning into a multi-gigabyte reserve.
// The largest shipped Bethesda ABC (Fallout 4's PipboyMenu) is well under this.
constexpr u32 kMaxPoolEntries = 1u << 20;

// The variable-length integer the whole format is built from: seven bits per
// byte, low group first, at most five bytes.
class AbcReader {
 public:
  explicit AbcReader(ByteSpan data) : data_(data) {}

  bool ok() const { return ok_; }
  mem_size pos() const { return pos_; }
  bool eof() const { return pos_ >= data_.size(); }

  u8 U8() {
    if (pos_ >= data_.size()) {
      ok_ = false;
      return 0;
    }
    return data_[pos_++];
  }

  u32 U30() {
    u32 value = 0;
    for (int shift = 0; shift < 35; shift += 7) {
      const u8 byte = U8();
      value |= static_cast<u32>(byte & 0x7f) << shift;
      if (!(byte & 0x80))
        break;
    }
    return value;
  }

  i32 S32() { return static_cast<i32>(U30()); }

  u16 U16() {
    const u16 lo = U8();
    return static_cast<u16>(lo | (static_cast<u16>(U8()) << 8));
  }

  f64 D64() {
    u8 bytes[8];
    for (int i = 0; i < 8; ++i)
      bytes[i] = U8();
    f64 out = 0;
    std::memcpy(&out, bytes, sizeof(out));
    return out;
  }

  base::String Utf8() {
    const u32 length = U30();
    base::String out;
    if (!ok_ || pos_ + length > data_.size()) {
      ok_ = false;
      return out;
    }
    for (u32 i = 0; i < length; ++i)
      out.push_back(static_cast<char>(data_[pos_ + i]));
    pos_ += length;
    return out;
  }

  ByteSpan Bytes(mem_size n) {
    if (pos_ + n > data_.size()) {
      ok_ = false;
      return {};
    }
    const ByteSpan out = data_.subspan(pos_, n);
    pos_ += n;
    return out;
  }

  void Skip(mem_size n) {
    if (pos_ + n > data_.size())
      ok_ = false;
    else
      pos_ += n;
  }

  bool Count(u32& out) {
    out = U30();
    if (!ok_ || out > kMaxPoolEntries) {
      ok_ = false;
      return false;
    }
    return true;
  }

 private:
  ByteSpan data_;
  mem_size pos_ = 0;
  bool ok_ = true;
};

struct Namespace {
  u8 kind = 0;
  u32 name = 0;  // string index
};

struct Multiname {
  u8 kind = 0;
  u32 name = 0;       // string index
  u32 ns = 0;         // namespace index
  u32 ns_set = 0;     // ns set index
  u32 type_name = 0;  // TypeName: the generic base
};

// Everything needed to turn pool indices back into readable names.
struct Pools {
  base::Vector<base::String> strings;
  base::Vector<Namespace> namespaces;
  base::Vector<Multiname> multinames;

  base::StringRef String(u32 index) const {
    return index < strings.size() ? base::StringRef(strings[index]) : base::StringRef();
  }

  base::String Name(u32 index, u32 depth = 0) const {
    if (index == 0 || index >= multinames.size())
      return base::String("*");  // index 0 is the "any" name
    const Multiname& m = multinames[index];
    switch (m.kind) {
      case 0x07:  // QName
      case 0x0d: {
        base::String out;
        if (m.ns < namespaces.size()) {
          const base::StringRef package = String(namespaces[m.ns].name);
          if (!package.empty()) {
            out += package;
            out += '.';
          }
        }
        out += String(m.name);
        return out;
      }
      case 0x09:  // Multiname
      case 0x0e:
        return base::String(String(m.name));
      case 0x0f:  // RTQName
      case 0x10:
        return base::String(String(m.name));
      case 0x1d: {  // TypeName, e.g. Vector.<String>
        // A TypeName names another multiname, and nothing in the format stops
        // that chain from looping back on itself.
        if (depth >= 16)
          return base::String("*");
        base::String out = Name(m.type_name, depth + 1);
        out += ".<>";
        return out;
      }
      default:
        return base::String("*");
    }
  }
};

struct AbcOp {
  u8 code;
  const char* name;
  // One character per operand:
  //   m multiname   s string   i int   u uint   d double   c class
  //   n method      r u30      b u8    j s24 branch
  //   a argument count (u30)   L lookupswitch  D debug
  const char* operands;
};

constexpr AbcOp kOps[] = {
    {0x01, "bkpt", ""},
    {0x02, "nop", ""},
    {0x03, "throw", ""},
    {0x04, "getsuper", "m"},
    {0x05, "setsuper", "m"},
    {0x06, "dxns", "s"},
    {0x07, "dxnslate", ""},
    {0x08, "kill", "r"},
    {0x09, "label", ""},
    {0x0c, "ifnlt", "j"},
    {0x0d, "ifnle", "j"},
    {0x0e, "ifngt", "j"},
    {0x0f, "ifnge", "j"},
    {0x10, "jump", "j"},
    {0x11, "iftrue", "j"},
    {0x12, "iffalse", "j"},
    {0x13, "ifeq", "j"},
    {0x14, "ifne", "j"},
    {0x15, "iflt", "j"},
    {0x16, "ifle", "j"},
    {0x17, "ifgt", "j"},
    {0x18, "ifge", "j"},
    {0x19, "ifstricteq", "j"},
    {0x1a, "ifstrictne", "j"},
    {0x1b, "lookupswitch", "L"},
    {0x1c, "pushwith", ""},
    {0x1d, "popscope", ""},
    {0x1e, "nextname", ""},
    {0x1f, "hasnext", ""},
    {0x20, "pushnull", ""},
    {0x21, "pushundefined", ""},
    {0x23, "nextvalue", ""},
    {0x24, "pushbyte", "b"},
    {0x25, "pushshort", "r"},
    {0x26, "pushtrue", ""},
    {0x27, "pushfalse", ""},
    {0x28, "pushnan", ""},
    {0x29, "pop", ""},
    {0x2a, "dup", ""},
    {0x2b, "swap", ""},
    {0x2c, "pushstring", "s"},
    {0x2d, "pushint", "i"},
    {0x2e, "pushuint", "u"},
    {0x2f, "pushdouble", "d"},
    {0x30, "pushscope", ""},
    {0x31, "pushnamespace", "r"},
    {0x32, "hasnext2", "rr"},
    {0x40, "newfunction", "n"},
    {0x41, "call", "a"},
    {0x42, "construct", "a"},
    {0x43, "callmethod", "ra"},
    {0x44, "callstatic", "na"},
    {0x45, "callsuper", "ma"},
    {0x46, "callproperty", "ma"},
    {0x47, "returnvoid", ""},
    {0x48, "returnvalue", ""},
    {0x49, "constructsuper", "a"},
    {0x4a, "constructprop", "ma"},
    {0x4c, "callproplex", "ma"},
    {0x4e, "callsupervoid", "ma"},
    {0x4f, "callpropvoid", "ma"},
    {0x53, "applytype", "a"},
    {0x55, "newobject", "a"},
    {0x56, "newarray", "a"},
    {0x57, "newactivation", ""},
    {0x58, "newclass", "c"},
    {0x59, "getdescendants", "m"},
    {0x5a, "newcatch", "r"},
    {0x5d, "findpropstrict", "m"},
    {0x5e, "findproperty", "m"},
    {0x5f, "finddef", "m"},
    {0x60, "getlex", "m"},
    {0x61, "setproperty", "m"},
    {0x62, "getlocal", "r"},
    {0x63, "setlocal", "r"},
    {0x64, "getglobalscope", ""},
    {0x65, "getscopeobject", "b"},
    {0x66, "getproperty", "m"},
    {0x68, "initproperty", "m"},
    {0x6a, "deleteproperty", "m"},
    {0x6c, "getslot", "r"},
    {0x6d, "setslot", "r"},
    {0x6e, "getglobalslot", "r"},
    {0x6f, "setglobalslot", "r"},
    {0x70, "convert_s", ""},
    {0x71, "esc_xelem", ""},
    {0x72, "esc_xattr", ""},
    {0x73, "convert_i", ""},
    {0x74, "convert_u", ""},
    {0x75, "convert_d", ""},
    {0x76, "convert_b", ""},
    {0x77, "convert_o", ""},
    {0x78, "checkfilter", ""},
    {0x80, "coerce", "m"},
    {0x82, "coerce_a", ""},
    {0x85, "coerce_s", ""},
    {0x86, "astype", "m"},
    {0x87, "astypelate", ""},
    {0x90, "negate", ""},
    {0x91, "increment", ""},
    {0x92, "inclocal", "r"},
    {0x93, "decrement", ""},
    {0x94, "declocal", "r"},
    {0x95, "typeof", ""},
    {0x96, "not", ""},
    {0x97, "bitnot", ""},
    {0xa0, "add", ""},
    {0xa1, "subtract", ""},
    {0xa2, "multiply", ""},
    {0xa3, "divide", ""},
    {0xa4, "modulo", ""},
    {0xa5, "lshift", ""},
    {0xa6, "rshift", ""},
    {0xa7, "urshift", ""},
    {0xa8, "bitand", ""},
    {0xa9, "bitor", ""},
    {0xaa, "bitxor", ""},
    {0xab, "equals", ""},
    {0xac, "strictequals", ""},
    {0xad, "lessthan", ""},
    {0xae, "lessequals", ""},
    {0xaf, "greaterthan", ""},
    {0xb0, "greaterequals", ""},
    {0xb1, "instanceof", ""},
    {0xb2, "istype", "m"},
    {0xb3, "istypelate", ""},
    {0xb4, "in", ""},
    {0xc0, "increment_i", ""},
    {0xc1, "decrement_i", ""},
    {0xc2, "inclocal_i", "r"},
    {0xc3, "declocal_i", "r"},
    {0xc4, "negate_i", ""},
    {0xc5, "add_i", ""},
    {0xc6, "subtract_i", ""},
    {0xc7, "multiply_i", ""},
    {0xd0, "getlocal_0", ""},
    {0xd1, "getlocal_1", ""},
    {0xd2, "getlocal_2", ""},
    {0xd3, "getlocal_3", ""},
    {0xd4, "setlocal_0", ""},
    {0xd5, "setlocal_1", ""},
    {0xd6, "setlocal_2", ""},
    {0xd7, "setlocal_3", ""},
    {0xef, "debug", "D"},
    {0xf0, "debugline", "r"},
    {0xf1, "debugfile", "s"},
    {0xf2, "bkptline", "r"},
    {0xf3, "timestamp", ""},
};

const AbcOp* FindOp(u8 code) {
  for (const AbcOp& op : kOps)
    if (op.code == code)
      return &op;
  return nullptr;
}

void ReadTraits(AbcReader& r, const Pools& pools, base::Vector<AbcTrait>& out) {
  u32 count = 0;
  if (!r.Count(count))
    return;
  for (u32 i = 0; i < count && r.ok(); ++i) {
    AbcTrait trait;
    const u32 name = r.U30();
    trait.name = pools.Name(name);
    const u8 tag = r.U8();
    const u8 kind = tag & 0x0f;
    const u8 attributes = static_cast<u8>(tag >> 4);
    trait.is_final = (attributes & 0x01) != 0;
    trait.is_override = (attributes & 0x02) != 0;

    switch (kind) {
      case 0:
      case 6: {  // slot / const
        trait.kind = kind == 0 ? TraitKind::kSlot : TraitKind::kConst;
        r.U30();  // slot id
        trait.type = pools.Name(r.U30());
        const u32 vindex = r.U30();
        if (vindex != 0)
          r.U8();  // value kind
        break;
      }
      case 1:
      case 2:
      case 3:
        trait.kind = kind == 1   ? TraitKind::kMethod
                     : kind == 2 ? TraitKind::kGetter
                                 : TraitKind::kSetter;
        r.U30();  // disp id
        trait.method = r.U30();
        break;
      case 4:
        trait.kind = TraitKind::kClass;
        r.U30();  // slot id
        trait.class_index = r.U30();
        break;
      case 5:
        trait.kind = TraitKind::kFunction;
        r.U30();  // slot id
        trait.method = r.U30();
        break;
      default:
        return;  // unknown trait kind: the rest of the stream is unreadable
    }

    if (attributes & 0x04) {  // has metadata
      u32 metadata_count = 0;
      if (!r.Count(metadata_count))
        return;
      for (u32 k = 0; k < metadata_count; ++k)
        r.U30();
    }
    out.push_back(base::move(trait));
  }
}

const char* KindKeyword(TraitKind kind) {
  switch (kind) {
    case TraitKind::kSlot:
      return "var";
    case TraitKind::kConst:
      return "const";
    case TraitKind::kMethod:
      return "function";
    case TraitKind::kGetter:
      return "function get";
    case TraitKind::kSetter:
      return "function set";
    case TraitKind::kClass:
      return "class";
    case TraitKind::kFunction:
      return "function";
  }
  return "var";
}

base::String Signature(const AbcFile& abc, const AbcTrait& trait) {
  base::String out;
  if (trait.is_override)
    out += "override ";
  if (trait.is_static)
    out += "static ";
  out += KindKeyword(trait.kind);
  out += ' ';
  out += trait.name;
  if (trait.kind == TraitKind::kSlot || trait.kind == TraitKind::kConst) {
    out += ':';
    out += trait.type;
    out += ';';
    return out;
  }
  if (trait.method >= abc.methods.size()) {
    out += "();";
    return out;
  }
  const AbcMethod& method = abc.methods[trait.method];
  out += '(';
  for (mem_size i = 0; i < method.param_types.size(); ++i) {
    if (i)
      out += ", ";
    if (i < method.param_names.size() && !method.param_names[i].empty())
      out += method.param_names[i];
    else
      out += base::Format("arg{}", i + 1);
    out += ':';
    out += method.param_types[i];
  }
  if (method.needs_rest)
    out += method.param_types.empty() ? "...rest" : ", ...rest";
  out += "):";
  out += method.return_type;
  return out;
}

void AppendMethodBody(const AbcFile& abc, u32 method_index, u32 indent,
                      base::String& out) {
  if (method_index >= abc.methods.size())
    return;
  const AbcMethod& method = abc.methods[method_index];
  if (method.body >= abc.bodies.size())
    return;
  const AbcMethodBody& body = abc.bodies[method.body];

  AbcReader r(body.code);
  while (r.ok() && !r.eof()) {
    const u32 offset = static_cast<u32>(r.pos());
    const u8 code = r.U8();
    const AbcOp* op = FindOp(code);
    for (u32 i = 0; i < indent; ++i)
      out += "  ";
    if (!op) {
      out += base::Format("{:04x}  .byte 0x{:02x}\n", offset, code);
      break;  // an unknown opcode desynchronises the stream
    }
    out += base::Format("{:04x}  {}", offset, op->name);
    for (const char* operand = op->operands; *operand; ++operand) {
      out += ' ';
      switch (*operand) {
        case 'j': {
          i32 delta = 0;
          delta |= r.U8();
          delta |= static_cast<i32>(r.U8()) << 8;
          delta |= static_cast<i32>(r.U8()) << 16;
          if (delta & 0x00800000)
            delta |= static_cast<i32>(0xff000000u);
          out += base::Format("{:04x}", static_cast<u32>(r.pos()) + delta);
          break;
        }
        case 'b':
          out += base::Format("{}", r.U8());
          break;
        case 'L': {
          i32 base_target = 0;
          base_target |= r.U8();
          base_target |= static_cast<i32>(r.U8()) << 8;
          base_target |= static_cast<i32>(r.U8()) << 16;
          const u32 case_count = r.U30();
          out += base::Format("{} case(s)", case_count + 1);
          for (u32 i = 0; i <= case_count && r.ok(); ++i)
            r.Skip(3);
          (void)base_target;
          break;
        }
        case 'D':
          r.U8();
          r.U30();
          r.U8();
          r.U30();
          break;
        default: {
          const u32 index = r.U30();
          if (*operand == 's')
            out += base::Format("\"{}\"", index < abc.strings.size()
                                              ? base::StringRef(abc.strings[index])
                                              : base::StringRef());
          else if (*operand == 'm' && index < abc.names.size())
            out += abc.names[index];
          else if (*operand == 'i' && index < abc.ints.size())
            out += base::Format("{}", abc.ints[index]);
          else if (*operand == 'd' && index < abc.doubles.size())
            out += base::Format("{}", abc.doubles[index]);
          else if (*operand == 'n' && index < abc.methods.size())
            out += base::Format("method#{} {}", index, abc.methods[index].name);
          else
            out += base::Format("{}", index);
          break;
        }
      }
    }
    out += '\n';
  }
}

void AppendClass(const AbcFile& abc, const AbcClass& klass, bool with_bodies,
                 base::String& out) {
  out += klass.is_interface ? "interface " : "class ";
  out += klass.name;
  if (!klass.super.empty() && klass.super != "*") {
    out += " extends ";
    out += klass.super;
  }
  for (mem_size i = 0; i < klass.interfaces.size(); ++i) {
    out += i == 0 ? " implements " : ", ";
    out += klass.interfaces[i];
  }
  out += " {\n";

  for (const AbcTrait& trait : klass.static_traits) {
    out += "  ";
    out += Signature(abc, trait);
    out += '\n';
    if (with_bodies && trait.kind != TraitKind::kSlot && trait.kind != TraitKind::kConst)
      AppendMethodBody(abc, trait.method, 2, out);
  }
  for (const AbcTrait& trait : klass.instance_traits) {
    out += "  ";
    out += Signature(abc, trait);
    out += '\n';
    if (with_bodies && trait.kind != TraitKind::kSlot && trait.kind != TraitKind::kConst)
      AppendMethodBody(abc, trait.method, 2, out);
  }
  out += "}\n\n";
}

base::String Render(const AbcFile& abc, bool with_bodies) {
  base::String out;
  if (!abc.name.empty())
    out += base::Format("// abc block \"{}\": {} classes, {} methods\n\n", abc.name,
                        abc.classes.size(), abc.methods.size());
  for (const AbcClass& klass : abc.classes)
    AppendClass(abc, klass, with_bodies, out);
  return out;
}

}  // namespace

base::StringRef AbcOpName(u8 op) {
  const AbcOp* found = FindOp(op);
  return found ? base::StringRef(found->name) : base::StringRef();
}

base::Vector<AbcInstruction> DisassembleMethod(const AbcMethodBody& body) {
  base::Vector<AbcInstruction> out;
  AbcReader r(body.code);
  while (r.ok() && !r.eof()) {
    AbcInstruction insn;
    insn.offset = static_cast<u32>(r.pos());
    insn.op = r.U8();
    const AbcOp* op = FindOp(insn.op);
    if (!op)
      break;  // an unknown opcode desynchronises the stream
    u32 filled = 0;
    for (const char* operand = op->operands; *operand && r.ok(); ++operand) {
      switch (*operand) {
        case 'j': {
          const u32 lo = r.U8();
          const u32 mid = r.U8();
          const u32 hi = r.U8();
          i32 value = static_cast<i32>(lo | (mid << 8) | (hi << 16));
          if (value & 0x00800000)
            value |= static_cast<i32>(0xff000000u);
          insn.jump = value;
          break;
        }
        case 'b':
          insn.a = r.U8();
          break;
        case 'L': {
          const u32 lo = r.U8();
          const u32 mid = r.U8();
          const u32 hi = r.U8();
          i32 fallback = static_cast<i32>(lo | (mid << 8) | (hi << 16));
          if (fallback & 0x00800000)
            fallback |= static_cast<i32>(0xff000000u);
          const u32 count = r.U30();
          for (u32 i = 0; i <= count && r.ok(); ++i) {
            const u32 a0 = r.U8();
            const u32 a1 = r.U8();
            const u32 a2 = r.U8();
            i32 target = static_cast<i32>(a0 | (a1 << 8) | (a2 << 16));
            if (target & 0x00800000)
              target |= static_cast<i32>(0xff000000u);
            insn.cases.push_back(target);
          }
          insn.cases.push_back(fallback);  // the default comes last
          break;
        }
        case 'D':
          r.U8();
          r.U30();
          r.U8();
          r.U30();
          break;
        default: {
          const u32 value = r.U30();
          if (filled == 0)
            insn.a = value;
          else
            insn.b = value;
          ++filled;
          break;
        }
      }
    }
    if (!r.ok())
      break;
    insn.end = static_cast<u32>(r.pos());
    out.push_back(base::move(insn));
  }
  return out;
}


base::Vector<ListBinding> ParseListBindings(const AbcFile& abc) {
  base::Vector<ListBinding> out;
  // Flash puts each instance's component-property setter in the class that owns
  // the instance, so the class the method belongs to names the owning symbol.
  for (const AbcClass& klass : abc.classes) {
    base::Vector<u32> methods;
    methods.push_back(klass.constructor);
    for (const AbcTrait& trait : klass.instance_traits)
      if (trait.kind == TraitKind::kMethod)
        methods.push_back(trait.method);

    for (u32 method_index : methods) {
      if (method_index >= abc.methods.size())
        continue;
      const AbcMethod& method = abc.methods[method_index];
      if (method.body >= abc.bodies.size())
        continue;

      // A tiny peephole over the op stream: remember the last property read and
      // the last literal pushed, and act when a setproperty names one of the
      // two properties that carry the wiring.
      base::String last_property;
      base::String last_string;
      u32 last_int = 0;
      AbcReader r(abc.bodies[method.body].code);
      while (r.ok() && !r.eof()) {
        const u8 code = r.U8();
        const AbcOp* op = FindOp(code);
        if (!op)
          break;  // an unknown opcode desynchronises the stream
        base::String name;
        u32 pushed = 0;
        bool has_string = false;
        for (const char* operand = op->operands; *operand; ++operand) {
          switch (*operand) {
            case 'j':
              r.U8();
              r.U8();
              r.U8();
              break;
            case 'b':
              pushed = r.U8();
              break;
            case 'L': {
              r.U8();
              r.U8();
              r.U8();
              const u32 count = r.U30();
              for (u32 i = 0; i <= count && r.ok(); ++i) {
                r.U8();
                r.U8();
                r.U8();
              }
              break;
            }
            case 'D':
              r.U8();
              r.U30();
              r.U8();
              r.U30();
              break;
            default: {
              const u32 index = r.U30();
              if (*operand == 's' && index < abc.strings.size()) {
                name = abc.strings[index];
                has_string = true;
              } else if (*operand == 'm' && index < abc.names.size()) {
                name = abc.names[index];
              } else if (*operand == 'i' && index < abc.ints.size()) {
                pushed = static_cast<u32>(abc.ints[index]);
              }
              break;
            }
          }
        }
        if (!r.ok())
          break;

        const base::StringRef op_name(op->name);
        if (op_name == "getproperty") {
          last_property = name;
        } else if (op_name == "pushstring" && has_string) {
          last_string = name;
        } else if (op_name == "pushbyte" || op_name == "pushshort" ||
                   op_name == "pushint") {
          last_int = pushed;
        } else if (op_name == "setproperty" && !last_property.empty()) {
          if (name == "listEntryClass" && !last_string.empty()) {
            ListBinding binding;
            binding.owner = klass.name;
            binding.instance = last_property;
            binding.entry = last_string;
            out.push_back(base::move(binding));
          } else if (name == "numListItems") {
            for (mem_size i = out.size(); i-- > 0;) {
              if (out[i].owner == klass.name && out[i].instance == last_property) {
                out[i].count = last_int;
                break;
              }
            }
          }
        }
      }
    }
  }
  return out;
}

base::StringRef Avm2OpName(u8 code) {
  const AbcOp* op = FindOp(code);
  return op ? base::StringRef(op->name) : base::StringRef("unknown");
}

bool ParseAbc(ByteSpan body, AbcFile& out) {
  AbcReader r(body);
  r.U16();  // DoABC flags (low half)
  r.U16();  // DoABC flags (high half)
  out.name = r.Utf8();

  r.U16();  // minor version
  r.U16();  // major version

  Pools pools;
  u32 count = 0;

  // Every pool is one-based: entry zero is the format's implicit default.
  if (!r.Count(count))
    return false;
  out.ints.push_back(0);
  for (u32 i = 1; i < count && r.ok(); ++i)
    out.ints.push_back(r.S32());
  if (!r.Count(count))
    return false;
  for (u32 i = 1; i < count && r.ok(); ++i)
    r.U30();
  if (!r.Count(count))
    return false;
  out.doubles.push_back(0);
  for (u32 i = 1; i < count && r.ok(); ++i)
    out.doubles.push_back(r.D64());

  if (!r.Count(count))
    return false;
  pools.strings.push_back(base::String());
  for (u32 i = 1; i < count && r.ok(); ++i)
    pools.strings.push_back(r.Utf8());

  if (!r.Count(count))
    return false;
  pools.namespaces.push_back(Namespace{});
  for (u32 i = 1; i < count && r.ok(); ++i) {
    Namespace ns;
    ns.kind = r.U8();
    ns.name = r.U30();
    pools.namespaces.push_back(ns);
  }

  if (!r.Count(count))
    return false;
  for (u32 i = 1; i < count && r.ok(); ++i) {
    u32 members = 0;
    if (!r.Count(members))
      return false;
    for (u32 k = 0; k < members && r.ok(); ++k)
      r.U30();
  }

  if (!r.Count(count))
    return false;
  pools.multinames.push_back(Multiname{});
  for (u32 i = 1; i < count && r.ok(); ++i) {
    Multiname m;
    m.kind = r.U8();
    switch (m.kind) {
      case 0x07:
      case 0x0d:
        m.ns = r.U30();
        m.name = r.U30();
        break;
      case 0x0f:
      case 0x10:
        m.name = r.U30();
        break;
      case 0x11:
      case 0x12:
        break;
      case 0x09:
      case 0x0e:
        m.name = r.U30();
        m.ns_set = r.U30();
        break;
      case 0x1b:
      case 0x1c:
        m.ns_set = r.U30();
        break;
      case 0x1d: {
        m.type_name = r.U30();
        u32 params = 0;
        if (!r.Count(params))
          return false;
        for (u32 k = 0; k < params && r.ok(); ++k)
          r.U30();
        break;
      }
      default:
        return false;  // unknown multiname kind: nothing after this is readable
    }
    pools.multinames.push_back(m);
  }
  out.strings = pools.strings;
  for (u32 i = 0; i < pools.multinames.size(); ++i)
    out.names.push_back(pools.Name(i));

  // Methods.
  if (!r.Count(count))
    return false;
  for (u32 i = 0; i < count && r.ok(); ++i) {
    AbcMethod method;
    u32 param_count = 0;
    if (!r.Count(param_count))
      return false;
    method.return_type = pools.Name(r.U30());
    for (u32 k = 0; k < param_count && r.ok(); ++k)
      method.param_types.push_back(pools.Name(r.U30()));
    method.name = base::String(pools.String(r.U30()));
    const u8 flags = r.U8();
    method.needs_rest = (flags & 0x04) != 0;
    if (flags & 0x08) {  // has optional parameter defaults
      u32 optional = 0;
      if (!r.Count(optional))
        return false;
      for (u32 k = 0; k < optional && r.ok(); ++k) {
        r.U30();
        r.U8();
      }
    }
    if (flags & 0x80) {  // has parameter names
      for (u32 k = 0; k < param_count && r.ok(); ++k)
        method.param_names.push_back(base::String(pools.String(r.U30())));
    }
    out.methods.push_back(base::move(method));
  }

  // Metadata: names only matter to tooling, so it is stepped over.
  if (!r.Count(count))
    return false;
  for (u32 i = 0; i < count && r.ok(); ++i) {
    r.U30();
    u32 items = 0;
    if (!r.Count(items))
      return false;
    for (u32 k = 0; k < items && r.ok(); ++k) {
      r.U30();
      r.U30();
    }
  }

  // Instances, then the class halves that pair with them.
  u32 class_count = 0;
  if (!r.Count(class_count))
    return false;
  for (u32 i = 0; i < class_count && r.ok(); ++i) {
    AbcClass klass;
    klass.name = pools.Name(r.U30());
    klass.super = pools.Name(r.U30());
    const u8 flags = r.U8();
    klass.sealed = (flags & 0x01) != 0;
    klass.is_interface = (flags & 0x04) != 0;
    if (flags & 0x08)
      r.U30();  // protected namespace
    u32 interfaces = 0;
    if (!r.Count(interfaces))
      return false;
    for (u32 k = 0; k < interfaces && r.ok(); ++k)
      klass.interfaces.push_back(pools.Name(r.U30()));
    klass.constructor = r.U30();
    ReadTraits(r, pools, klass.instance_traits);
    out.classes.push_back(base::move(klass));
  }
  for (u32 i = 0; i < class_count && r.ok(); ++i) {
    AbcClass& klass = out.classes[i];
    klass.static_initializer = r.U30();
    ReadTraits(r, pools, klass.static_traits);
    for (AbcTrait& trait : klass.static_traits)
      trait.is_static = true;
  }

  // Scripts: package-level code. Their traits name the classes, which the
  // outline already covers, so only the shape is stepped over.
  if (!r.Count(count))
    return false;
  for (u32 i = 0; i < count && r.ok(); ++i) {
    r.U30();  // init method
    base::Vector<AbcTrait> traits;
    ReadTraits(r, pools, traits);
  }

  // Method bodies.
  if (!r.Count(count))
    return false;
  for (u32 i = 0; i < count && r.ok(); ++i) {
    AbcMethodBody body;
    body.method = r.U30();
    body.max_stack = r.U30();
    body.local_count = r.U30();
    r.U30();  // init scope depth
    r.U30();  // max scope depth
    u32 code_length = 0;
    if (!r.Count(code_length))
      return false;
    body.code = r.Bytes(code_length);
    u32 exceptions = 0;
    if (!r.Count(exceptions))
      return false;
    for (u32 k = 0; k < exceptions && r.ok(); ++k) {
      r.U30();
      r.U30();
      r.U30();
      r.U30();
      r.U30();
    }
    base::Vector<AbcTrait> traits;
    ReadTraits(r, pools, traits);
    if (body.method < out.methods.size())
      out.methods[body.method].body = static_cast<u32>(out.bodies.size());
    out.bodies.push_back(base::move(body));
  }

  return r.ok();
}

base::String AbcOutline(const AbcFile& abc) {
  return Render(abc, false);
}

base::String AbcDisassembly(const AbcFile& abc) {
  return Render(abc, true);
}

}  // namespace rx::swf
