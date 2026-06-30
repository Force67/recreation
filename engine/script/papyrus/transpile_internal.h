#ifndef RECREATION_SCRIPT_PAPYRUS_TRANSPILE_INTERNAL_H_
#define RECREATION_SCRIPT_PAPYRUS_TRANSPILE_INTERNAL_H_

#include <cctype>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "script/papyrus/opcode.h"
#include "script/papyrus/pex.h"
#include "script/papyrus/transpile.h"

// Helpers shared by the two halves of the transpiler: decompiler.cc (one
// function body to C# statements) and transpile.cc (the surrounding class and
// file structure). They are inline so both translation units agree on them.
namespace rec::script::papyrus::detail {

inline const std::unordered_set<std::string>& CsKeywords() {
  static const std::unordered_set<std::string> kKeywords = {
      "abstract", "as",      "base",    "bool",    "break",   "byte",    "case",
      "catch",    "char",    "checked", "class",   "const",   "continue", "decimal",
      "default",  "delegate","do",      "double",  "else",    "enum",    "event",
      "explicit", "extern",  "false",   "finally", "fixed",   "float",   "for",
      "foreach",  "goto",    "if",      "implicit","in",      "int",     "interface",
      "internal", "is",      "lock",    "long",    "namespace","new",    "null",
      "object",   "operator","out",     "override","params",  "private", "protected",
      "public",   "readonly","ref",     "return",  "sbyte",   "sealed",  "short",
      "sizeof",   "static",  "string",  "struct",  "switch",  "this",    "throw",
      "true",     "try",     "typeof",  "uint",    "ulong",   "unchecked","unsafe",
      "ushort",   "using",   "virtual", "void",    "volatile","while",   "value",
  };
  return kKeywords;
}

// Turns a Papyrus identifier into a legal C# one. The compiler emits names like
// "::temp3" and "::Foo_var"; both become legal fields or locals here.
inline std::string Sanitize(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name)
    out.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_');
  if (out.empty()) out = "_";
  if (std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), '_');
  if (CsKeywords().count(out)) out.push_back('_');
  return out;
}

inline std::string EscapeString(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:   out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

inline std::string FormatFloat(f32 v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
  std::string s = buf;
  if (s.find_first_of(".eEnN") == std::string::npos) s += ".0";  // keep it a float literal
  s += "f";
  return s;
}

inline bool IEquals(const std::string& a, const char* b) {
  size_t i = 0;
  for (; i < a.size() && b[i]; ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return i == a.size() && b[i] == '\0';
}

inline std::string ToLower(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Canonical PascalCase for the engine types the SDK ships. Papyrus is
// case-insensitive and the .pex stores whatever casing the author used (often
// lowercased), so a call would otherwise read "game.GetPlayer()" against a
// case-sensitive C# type. Keyed by the lowercased name.
inline const std::unordered_map<std::string, std::string>& CanonicalTypes() {
  static const std::unordered_map<std::string, std::string> kTypes = {
      {"game", "Game"}, {"debug", "Debug"}, {"utility", "Utility"}, {"math", "Math"},
      {"form", "Form"}, {"objectreference", "ObjectReference"}, {"actor", "Actor"},
      {"actorbase", "ActorBase"}, {"quest", "Quest"}, {"faction", "Faction"},
      {"cell", "Cell"}, {"location", "Location"}, {"worldspace", "WorldSpace"},
      {"race", "Race"}, {"spell", "Spell"}, {"shout", "Shout"}, {"perk", "Perk"},
      {"keyword", "Keyword"}, {"sound", "Sound"}, {"message", "Message"},
      {"musictype", "MusicType"}, {"idle", "Idle"}, {"package", "Package"},
      {"weapon", "Weapon"}, {"armor", "Armor"}, {"book", "Book"}, {"potion", "Potion"},
      {"ingredient", "Ingredient"}, {"enchantment", "Enchantment"}, {"scene", "Scene"},
      {"topic", "Topic"}, {"topicinfo", "TopicInfo"}, {"referencealias", "ReferenceAlias"},
      {"locationalias", "LocationAlias"}, {"alias", "Alias"}, {"globalvariable", "GlobalVariable"},
      {"formlist", "FormList"}, {"leveledactor", "LeveledActor"}, {"leveleditem", "LeveledItem"},
      {"magiceffect", "MagicEffect"}, {"activator", "Activator"}, {"container", "Container"},
      {"door", "Door"}, {"light", "Light"}, {"static", "Static"}, {"furniture", "Furniture"},
      {"explosion", "Explosion"}, {"hazard", "Hazard"}, {"projectile", "Projectile"},
      {"encounterzone", "EncounterZone"}, {"objectivehandler", "ObjectiveHandler"},
      {"scriptobject", "ScriptObject"}, {"actorvalue", "ActorValue"},
  };
  return kTypes;
}

// Maps a Papyrus type name to the closest SDK or C# type. Unknown object types
// pass through unchanged, since they name other decompiled script classes.
inline std::string CsType(const std::string& papyrus) {
  if (papyrus.empty() || IEquals(papyrus, "none")) return "void";
  if (papyrus.size() >= 2 && papyrus.compare(papyrus.size() - 2, 2, "[]") == 0)
    return CsType(papyrus.substr(0, papyrus.size() - 2)) + "[]";
  if (IEquals(papyrus, "int")) return "int";
  if (IEquals(papyrus, "float")) return "float";
  if (IEquals(papyrus, "bool")) return "bool";
  if (IEquals(papyrus, "string")) return "string";
  if (IEquals(papyrus, "var")) return "object";
  if (IEquals(papyrus, "self")) return "this";
  const auto& canon = CanonicalTypes();
  auto it = canon.find(ToLower(papyrus));
  return it != canon.end() ? it->second : Sanitize(papyrus);
}

// Type for a declaration. In the harness every type but `void` collapses to
// `dynamic`, so the standalone compile checks structure (control flow, scoping,
// returns) instead of Papyrus's loose coercions, which strict C# would reject.
inline std::string DeclTypeFor(const std::string& papyrus, bool harness) {
  std::string cs = CsType(papyrus);
  if (!harness) return cs;
  return cs == "void" ? cs : "dynamic";
}

// Roles of an instruction's fixed operands. dest is the arg index written, or
// -1. reads lists the arg indices that are variable reads, so name-only operands
// (a method name, a property name) are excluded. Call opcodes also read every
// var_arg.
struct Roles {
  int dest = -1;
  std::vector<int> reads;
  bool var_reads = false;
};

inline Roles RolesOf(Op op) {
  switch (op) {
    case Op::kIAdd: case Op::kFAdd: case Op::kISub: case Op::kFSub:
    case Op::kIMul: case Op::kFMul: case Op::kIDiv: case Op::kFDiv:
    case Op::kIMod: case Op::kStrCat:
    case Op::kCmpEq: case Op::kCmpLt: case Op::kCmpLe: case Op::kCmpGt: case Op::kCmpGe:
      return {0, {1, 2}, false};
    case Op::kNot: case Op::kINeg: case Op::kFNeg: case Op::kAssign: case Op::kCast:
      return {0, {1}, false};
    case Op::kJmp:      return {-1, {}, false};
    case Op::kJmpT: case Op::kJmpF: return {-1, {0}, false};
    case Op::kCallMethod: return {2, {1}, true};
    case Op::kCallParent: return {1, {}, true};
    case Op::kCallStatic: return {2, {}, true};
    case Op::kReturn:     return {-1, {0}, false};
    case Op::kPropGet:    return {2, {1}, false};
    case Op::kPropSet:    return {-1, {1, 2}, false};
    case Op::kArrayCreate: return {0, {1}, false};
    case Op::kArrayLength: return {0, {1}, false};
    case Op::kArrayGetElement: return {0, {1, 2}, false};
    case Op::kArraySetElement: return {-1, {0, 1, 2}, false};
    case Op::kArrayFindElement: case Op::kArrayRFindElement: return {1, {0, 2, 3}, false};
    case Op::kIs: return {0, {1}, false};
    default: return {-1, {}, false};
  }
}

inline bool HasSideEffect(Op op) {
  return op == Op::kCallMethod || op == Op::kCallParent || op == Op::kCallStatic;
}

inline int JumpRel(const Instruction& in) {
  switch (in.op) {
    case Op::kJmp:  return in.args.empty() ? 0 : in.args[0].int_value;
    case Op::kJmpT:
    case Op::kJmpF: return in.args.size() < 2 ? 0 : in.args[1].int_value;
    default:        return 0;
  }
}

inline bool IsBranch(Op op) { return op == Op::kJmp || op == Op::kJmpT || op == Op::kJmpF; }

// Whether control can fall off the end of the function on some reachable path.
// Papyrus lets a non-Void function run off the end and implicitly return the
// zero value, so when this is true the emitter adds a trailing `return default;`.
inline bool EndReachable(const Function& fn) {
  const auto& code = fn.code;
  const int n = static_cast<int>(code.size());
  if (n == 0) return true;
  std::vector<char> seen(static_cast<size_t>(n), 0);
  std::vector<int> stack = {0};
  bool reaches_end = false;
  while (!stack.empty()) {
    int i = stack.back();
    stack.pop_back();
    if (i == n) { reaches_end = true; continue; }
    if (i < 0 || i > n || seen[static_cast<size_t>(i)]) continue;
    seen[static_cast<size_t>(i)] = 1;
    const Instruction& in = code[i];
    switch (in.op) {
      case Op::kReturn: break;
      case Op::kJmp: stack.push_back(i + JumpRel(in)); break;
      case Op::kJmpT:
      case Op::kJmpF:
        stack.push_back(i + 1);
        stack.push_back(i + JumpRel(in));
        break;
      default: stack.push_back(i + 1); break;
    }
  }
  return reaches_end;
}

// The lookup tables the surrounding class builds for a single object, handed to
// the body decompiler. Any pointer may be null.
struct DecompileCtx {
  const PexFile& pex;
  const std::unordered_map<std::string, std::string>* backing;    // backing var -> property
  const std::unordered_map<std::string, std::string>* fn_rename;  // function -> renamed (field clash)
  const std::unordered_map<std::string, std::string>* case_names; // lower -> canonical member/property
  const std::unordered_map<std::string, std::string>* case_fns;   // lower -> canonical function
  const TranspileOptions* opts;
};

// Emits the statements of fn into out at the given indent. value_alias, when set,
// names a property setter's value parameter, which becomes C#'s implicit `value`.
void DecompileFunction(const DecompileCtx& ctx, const Function& fn, std::string& out, int indent,
                       const std::string& value_alias = {});

}  // namespace rec::script::papyrus::detail

#endif  // RECREATION_SCRIPT_PAPYRUS_TRANSPILE_INTERNAL_H_
