#ifndef RECREATION_SCRIPT_PAPYRUS_PEX_H_
#define RECREATION_SCRIPT_PAPYRUS_PEX_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "core/types.h"
#include "components/script/papyrus/opcode.h"

namespace rx::script::papyrus {

// Index into PexFile::string_table. The format stores every name and literal
// string once and references them by index, so the parsed model keeps indices
// rather than copying strings around.
using StringIndex = u16;
constexpr StringIndex kInvalidString = 0xFFFF;

// One operand or default value, mirroring the on-disk VariableData record. The
// active field is selected by type: identifier/string use string_index, the
// numeric and bool types use their own field.
struct VariableData {
  enum class Type : u8 {
    kNone = 0,
    kIdentifier = 1,  // names a local, parameter, or member variable
    kString = 2,
    kInteger = 3,
    kFloat = 4,
    kBool = 5,
  };
  Type type = Type::kNone;
  StringIndex string_index = kInvalidString;
  i32 int_value = 0;
  f32 float_value = 0;
  bool bool_value = false;
};

// A single decoded bytecode instruction. args holds the opcode's fixed
// operands; var_args holds the trailing argument list of a call opcode.
struct Instruction {
  Op op = Op::kNop;
  base::Vector<VariableData> args;
  base::Vector<VariableData> var_args;
};

// name + type, both string indices. Used for parameters, locals and members.
struct TypedName {
  StringIndex name = kInvalidString;
  StringIndex type = kInvalidString;
};

struct Function {
  StringIndex return_type = kInvalidString;
  StringIndex doc_string = kInvalidString;
  u32 user_flags = 0;
  bool is_global = false;
  bool is_native = false;
  base::Vector<TypedName> params;
  base::Vector<TypedName> locals;
  base::Vector<Instruction> code;
};

struct NamedFunction {
  StringIndex name = kInvalidString;
  Function function;
};

struct State {
  StringIndex name = kInvalidString;  // the empty string indexes the default state
  base::Vector<NamedFunction> functions;
};

struct Property {
  StringIndex name = kInvalidString;
  StringIndex type = kInvalidString;
  StringIndex doc_string = kInvalidString;
  u32 user_flags = 0;
  u8 flags = 0;  // bit0 readable, bit1 writable, bit2 auto (backed by a variable)
  StringIndex auto_var_name = kInvalidString;
  Function getter;
  Function setter;
  bool has_getter = false;
  bool has_setter = false;
  bool is_auto() const { return (flags & 0x4) != 0; }
  bool readable() const { return (flags & 0x1) != 0; }
  bool writable() const { return (flags & 0x2) != 0; }
};

struct MemberVariable {
  StringIndex name = kInvalidString;
  StringIndex type = kInvalidString;
  u32 user_flags = 0;
  VariableData initial_value;
};

struct Object {
  StringIndex name = kInvalidString;
  StringIndex parent_class = kInvalidString;
  StringIndex doc_string = kInvalidString;
  u32 user_flags = 0;
  StringIndex auto_state_name = kInvalidString;
  base::Vector<MemberVariable> variables;
  base::Vector<Property> properties;
  base::Vector<State> states;
};

struct DebugFunction {
  StringIndex object_name = kInvalidString;
  StringIndex state_name = kInvalidString;
  StringIndex function_name = kInvalidString;
  u8 function_type = 0;
  base::Vector<u16> line_numbers;
};

// A parsed .pex file: the immutable, game-agnostic program the VM links and
// runs. The same structure serves Skyrim and Fallout; only the byte order and
// a couple of optional debug sections differ, both handled by ParsePex.
struct PexFile {
  bool big_endian = true;  // Skyrim is big-endian on disk, Fallout little
  u8 major_version = 0;
  u8 minor_version = 0;
  u16 game_id = 0;  // 1 = Skyrim, 2 = Fallout 4
  u64 compilation_time = 0;
  base::String source_file_name;
  base::String user_name;
  base::String machine_name;

  base::Vector<base::String> string_table;

  bool has_debug_info = false;
  u64 modification_time = 0;
  base::Vector<DebugFunction> debug_functions;

  struct UserFlag {
    StringIndex name = kInvalidString;
    u8 bit_index = 0;
  };
  base::Vector<UserFlag> user_flags;

  base::Vector<Object> objects;

  // Resolves a string index. Out-of-range or kInvalidString yields "".
  const base::String& Str(StringIndex index) const;
};

// Parses a .pex blob into out. Detects byte order from the magic, validates
// every length against the buffer, and returns false (logging why) on a
// truncated or malformed stream. out is left partially filled on failure.
bool ParsePex(ByteSpan data, PexFile* out);

}  // namespace rx::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_PEX_H_
