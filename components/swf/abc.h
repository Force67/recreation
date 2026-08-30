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

// One decoded AVM2 instruction. The operands the menus use are all a pair of
// u30s at most (a multiname or register, then an argument count), plus a signed
// branch displacement measured from the end of the instruction.
struct AbcInstruction {
  u8 op = 0;
  u32 offset = 0;  // from the start of the method body
  u32 end = 0;     // one past its last byte, which is where a branch counts from
  u32 a = 0;
  u32 b = 0;
  i32 jump = 0;
  base::Vector<i32> cases;  // lookupswitch, default last
};

// Decodes a method body into instructions. An opcode the table does not know
// desynchronises the stream, so the decode stops there rather than guessing.
base::Vector<AbcInstruction> DisassembleMethod(const AbcMethodBody& body);

// The name an opcode goes by, for diagnostics. Empty when it is unknown.
base::StringRef AbcOpName(u8 op);

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

// How an ActionScript 3 list is wired to the rows it shows.
//
// The AS2 games place their list rows on the timeline, so a static translation
// finds them already there. AS3 does not: a list ships as an empty container and
// the code fills it, which is why a translated Fallout 4 menu comes out with the
// panel but no entries. The wiring is still in the bytecode as a literal, in the
// component-property setter Flash generates for the instance:
//
//   getproperty  List_mc
//   pushstring   "MainMenuListEntry"
//   setproperty  listEntryClass
//   getproperty  List_mc
//   pushbyte     9
//   setproperty  numListItems
//
// so the rows can be stamped out statically after all.
struct ListBinding {
  base::String owner;     // class holding the list, e.g. "MainMenu_fla.MainListPanel_49"
  base::String instance;  // the list's instance name, e.g. "List_mc"
  base::String entry;     // the row symbol to instantiate, e.g. "MainMenuListEntry"
  u32 count = 0;          // numListItems: how many rows fit at once
};

// Every list wiring the bytecode declares. Empty for a movie with no DoABC.
base::Vector<ListBinding> ParseListBindings(const AbcFile& abc);

}  // namespace rx::swf

#endif  // RECREATION_SWF_ABC_H_
