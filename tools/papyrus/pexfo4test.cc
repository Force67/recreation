// pexfo4test: deterministic check that ParsePex reads the Fallout 4 object
// layout, which differs from Skyrim by three fields (an object const flag, a
// struct-definition section, and a per-variable const flag). A hand-built
// little-endian FO4 .pex exercises all three; if any were mis-sized the function
// code would be reached at the wrong offset and its single Return would not
// decode. No game data needed, so it runs in the ctest gate. The Skyrim layout
// is covered by the existing tests that parse shipped Skyrim .pex.

#include <cstdio>
#include <vector>

#include "core/types.h"
#include "script/papyrus/pex.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Little-endian writers (Fallout .pex is little-endian on disk).
void PutU8(std::vector<rx::u8>& b, rx::u8 v) { b.push_back(v); }
void PutU16(std::vector<rx::u8>& b, rx::u16 v) {
  b.push_back(rx::u8(v));
  b.push_back(rx::u8(v >> 8));
}
void PutU32(std::vector<rx::u8>& b, rx::u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(rx::u8(v >> (8 * i)));
}
void PutWStr(std::vector<rx::u8>& b, const char* s) {
  rx::u16 n = 0;
  while (s[n]) ++n;
  PutU16(b, n);
  for (rx::u16 i = 0; i < n; ++i) b.push_back(static_cast<rx::u8>(s[i]));
}
// A kNone VariableData: one type byte, no payload.
void PutNoneValue(std::vector<rx::u8>& b) { PutU8(b, 0); }

}  // namespace

int main() {
  std::puts("fallout 4 pex object layout:");
  using namespace rx::script::papyrus;

  // String table indices used below.
  enum : rx::u16 {
    kEmpty = 0, kScript, kParent, kVar, kInt, kStruct, kMember, kFunc, kBool,
  };

  std::vector<rx::u8> b;
  b.insert(b.end(), {0xDE, 0xC0, 0x57, 0xFA});  // FO4 magic (little-endian)
  PutU8(b, 3);                                   // major
  PutU8(b, 9);                                   // minor
  PutU16(b, 2);                                  // game id: Fallout 4
  for (int i = 0; i < 8; ++i) PutU8(b, 0);       // compilation time
  PutWStr(b, "Test.psc");                         // source
  PutWStr(b, "user");
  PutWStr(b, "machine");

  PutU16(b, 9);  // string table count
  PutWStr(b, "");           // 0
  PutWStr(b, "MyScript");   // 1
  PutWStr(b, "ScriptObject");  // 2
  PutWStr(b, "MyVar");      // 3
  PutWStr(b, "Int");        // 4
  PutWStr(b, "MyStruct");   // 5
  PutWStr(b, "Member1");    // 6
  PutWStr(b, "DoStuff");    // 7
  PutWStr(b, "Bool");       // 8

  PutU8(b, 0);    // has debug info: none
  PutU16(b, 0);   // user flag count
  PutU16(b, 1);   // object count

  // --- Object (Fallout 4 layout) ---
  PutU16(b, kScript);  // name
  PutU32(b, 0);        // data size (the reader ignores it, parsing sequentially)
  PutU16(b, kParent);  // parent class
  PutU16(b, kEmpty);   // doc string
  PutU8(b, 0);         // FO4 object const flag
  PutU32(b, 0);        // user flags
  PutU16(b, kEmpty);   // auto state name

  // Struct section (FO4): one struct with one member.
  PutU16(b, 1);        // struct count
  PutU16(b, kStruct);  // struct name
  PutU16(b, 1);        // member count
  PutU16(b, kMember);  // member name
  PutU16(b, kInt);     // member type
  PutU32(b, 0);        // member user flags
  PutNoneValue(b);     // member default value
  PutU8(b, 0);         // member const flag
  PutU16(b, kEmpty);   // member doc string

  // Variables (FO4): one variable, with the trailing const flag.
  PutU16(b, 1);        // variable count
  PutU16(b, kVar);     // name
  PutU16(b, kInt);     // type
  PutU32(b, 0);        // user flags
  PutNoneValue(b);     // initial value
  PutU8(b, 0);         // FO4 variable const flag

  PutU16(b, 0);  // property count

  // States: the default state with one function holding a single Return.
  PutU16(b, 1);        // state count
  PutU16(b, kEmpty);   // state name (default)
  PutU16(b, 1);        // function count
  PutU16(b, kFunc);    // function name
  // Function body.
  PutU16(b, kBool);    // return type
  PutU16(b, kEmpty);   // doc string
  PutU32(b, 0);        // user flags
  PutU8(b, 0);         // flags (not global, not native)
  PutU16(b, 0);        // param count
  PutU16(b, 0);        // local count
  PutU16(b, 1);        // instruction count
  PutU8(b, static_cast<rx::u8>(Op::kReturn));  // opcode 0x1A, one operand
  PutNoneValue(b);     // the return operand (None)

  PexFile pex;
  const bool ok = ParsePex(rx::ByteSpan(b.data(), b.size()), &pex);
  Check("parses", ok);
  Check("little-endian fallout", !pex.big_endian);
  Check("game id 2", pex.game_id == 2);
  Check("one object", pex.objects.size() == 1);
  if (pex.objects.size() != 1) {
    std::printf("fallout 4 pex: %d checks FAILED\n", g_failures + 1);
    return 1;
  }
  const Object& o = pex.objects[0];
  Check("object name", pex.Str(o.name) == "MyScript");
  Check("parent resolved (struct section consumed)", pex.Str(o.parent_class) == "ScriptObject");
  Check("one variable (var const flag consumed)", o.variables.size() == 1);
  Check("variable name", o.variables.size() == 1 && pex.Str(o.variables[0].name) == "MyVar");
  Check("one state", o.states.size() == 1);
  const bool have_fn = o.states.size() == 1 && o.states[0].functions.size() == 1;
  Check("one function", have_fn);
  if (have_fn) {
    const NamedFunction& nf = o.states[0].functions[0];
    Check("function name", pex.Str(nf.name) == "DoStuff");
    Check("function code reached at the right offset", nf.function.code.size() == 1);
    Check("single Return decoded",
          nf.function.code.size() == 1 && nf.function.code[0].op == Op::kReturn);
  }

  if (g_failures == 0) {
    std::puts("fallout 4 pex: all checks passed");
    return 0;
  }
  std::printf("fallout 4 pex: %d checks FAILED\n", g_failures);
  return 1;
}
