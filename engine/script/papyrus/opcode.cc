#include "script/papyrus/opcode.h"

namespace rx::script::papyrus {
namespace {

// Indexed by opcode value. Arities for 0x00-0x23 are exact (Skyrim, validated
// against shipped .pex). The 0x24-0x2F Fallout entries are provisional: they
// keep the decoder from desyncing if a Fallout stream is ever fed in, but are
// not exercised by the Skyrim VM and get hardened when Fallout assets land.
constexpr OpInfo kTable[] = {
    {"nop", 0, false},                  // 0x00
    {"iadd", 3, false},                 // 0x01
    {"fadd", 3, false},                 // 0x02
    {"isub", 3, false},                 // 0x03
    {"fsub", 3, false},                 // 0x04
    {"imul", 3, false},                 // 0x05
    {"fmul", 3, false},                 // 0x06
    {"idiv", 3, false},                 // 0x07
    {"fdiv", 3, false},                 // 0x08
    {"imod", 3, false},                 // 0x09
    {"not", 2, false},                  // 0x0A
    {"ineg", 2, false},                 // 0x0B
    {"fneg", 2, false},                 // 0x0C
    {"assign", 2, false},               // 0x0D
    {"cast", 2, false},                 // 0x0E
    {"cmp_eq", 3, false},               // 0x0F
    {"cmp_lt", 3, false},               // 0x10
    {"cmp_le", 3, false},               // 0x11
    {"cmp_gt", 3, false},               // 0x12
    {"cmp_ge", 3, false},               // 0x13
    {"jmp", 1, false},                  // 0x14
    {"jmpt", 2, false},                 // 0x15
    {"jmpf", 2, false},                 // 0x16
    {"callmethod", 3, true},            // 0x17 name, self, dest, [args]
    {"callparent", 2, true},            // 0x18 name, dest, [args]
    {"callstatic", 3, true},            // 0x19 type, name, dest, [args]
    {"return", 1, false},               // 0x1A
    {"strcat", 3, false},               // 0x1B
    {"propget", 3, false},              // 0x1C
    {"propset", 3, false},              // 0x1D
    {"array_create", 2, false},         // 0x1E
    {"array_length", 2, false},         // 0x1F
    {"array_getelement", 3, false},     // 0x20
    {"array_setelement", 3, false},     // 0x21
    {"array_findelement", 4, false},    // 0x22
    {"array_rfindelement", 4, false},   // 0x23
    {"is", 3, false},                   // 0x24 (fo4)
    {"struct_create", 1, false},        // 0x25 (fo4)
    {"struct_get", 3, false},           // 0x26 (fo4)
    {"struct_set", 3, false},           // 0x27 (fo4)
    {"array_findstruct", 5, false},     // 0x28 (fo4)
    {"array_rfindstruct", 5, false},    // 0x29 (fo4)
    {"array_add", 3, false},            // 0x2A (fo4)
    {"array_insert", 3, false},         // 0x2B (fo4)
    {"array_removelast", 1, false},     // 0x2C (fo4)
    {"array_remove", 3, false},         // 0x2D (fo4)
    {"array_clear", 1, false},          // 0x2E (fo4)
    {"array_getallmatchingstructs", 5, false},  // 0x2F (fo76)
};

constexpr OpInfo kInvalid = {"invalid", 0, false};

}  // namespace

const OpInfo& GetOpInfo(Op op) {
  auto raw = static_cast<u8>(op);
  if (raw >= sizeof(kTable) / sizeof(kTable[0])) return kInvalid;
  return kTable[raw];
}

}  // namespace rx::script::papyrus
