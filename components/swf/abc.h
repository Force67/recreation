#ifndef RECREATION_SWF_ABC_H_
#define RECREATION_SWF_ABC_H_

#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace rx::swf {

// ActionScript 3 bytecode, as carried by a DoABC tag.
//
// Skyrim's menus are ActionScript 2 and decompile through avm1/decompile;
// Fallout 4 and Starfield moved to ActionScript 3, whose classes and methods
// live in this format instead. The reader below recovers the whole symbol
// structure (packages, classes, inheritance, members, method signatures) with
// real names, and disassembles the method bodies.

enum class TraitKind : u8 {
  kSlot = 0,
  kMethod = 1,
  kGetter = 2,
  kSetter = 3,
  kClass = 4,
  kFunction = 5,
  kConst = 6,
};

struct AbcTrait {
  TraitKind kind = TraitKind::kSlot;
  base::String name;      // resolved multiname
  base::String type;      // slots/consts: the declared type
  u32 method = 0;         // methods/getters/setters/functions: method index
  u32 class_index = 0;    // kClass
  bool is_static = false;
  bool is_final = false;
  bool is_override = false;
};

struct AbcMethod {
  base::String name;         // as authored; often empty for anonymous functions
  base::String return_type;
  base::Vector<base::String> param_types;
  base::Vector<base::String> param_names;
  bool needs_rest = false;
  u32 body = ~0u;  // index into AbcFile::bodies, ~0 when the method has none
};

struct AbcMethodBody {
  u32 method = 0;
  u32 max_stack = 0;
  u32 local_count = 0;
  ByteSpan code;  // points into the DoABC tag body
};

struct AbcClass {
  base::String name;   // fully qualified, e.g. "Shared.GlobalFunc"
  base::String super;  // fully qualified base class
  base::Vector<base::String> interfaces;
  bool sealed = false;
  bool is_interface = false;
  u32 constructor = 0;        // instance initialiser method index
  u32 static_initializer = 0;  // class initialiser method index
  base::Vector<AbcTrait> instance_traits;
  base::Vector<AbcTrait> static_traits;
};

struct AbcFile {
  base::String name;  // the DoABC tag's own label
  base::Vector<base::String> strings;
  // Multinames resolved to "package.Name", so an instruction operand prints the
  // property it touches instead of a pool index.
  base::Vector<base::String> names;
  base::Vector<i32> ints;
  base::Vector<f64> doubles;
  base::Vector<AbcMethod> methods;
  base::Vector<AbcMethodBody> bodies;
  base::Vector<AbcClass> classes;
};

// Reads a DoABC tag body (flags + name + the abcFile that follows). Returns
// false on a truncated or malformed pool; `out` then holds whatever was read
// before the damage, which is still worth printing.
bool ParseAbc(ByteSpan body, AbcFile& out);

// The class structure as ActionScript 3 declarations: packages, classes with
// their base and interfaces, and every member with its real name and type. This
// is the shape of the UI's code, without the statement bodies.
base::String AbcOutline(const AbcFile& abc);

// The same outline with every method body disassembled underneath it, one AVM2
// instruction per line with pool references resolved to names.
base::String AbcDisassembly(const AbcFile& abc);

base::StringRef Avm2OpName(u8 code);

}  // namespace rx::swf

#endif  // RECREATION_SWF_ABC_H_
