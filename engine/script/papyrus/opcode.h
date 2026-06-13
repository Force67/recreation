#ifndef RECREATION_SCRIPT_PAPYRUS_OPCODE_H_
#define RECREATION_SCRIPT_PAPYRUS_OPCODE_H_

#include "core/types.h"

namespace rec::script::papyrus {

// The Papyrus bytecode instruction set. Skyrim defines 0x00-0x23; Fallout 4
// and 76 append struct/array opcodes (0x24-0x37) that share this numbering, so
// the enum is the one place a later dialect plugs new opcodes in. The decoder
// and interpreter key off OpInfo, never off raw values.
enum class Op : u8 {
  kNop = 0x00,
  kIAdd = 0x01,
  kFAdd = 0x02,
  kISub = 0x03,
  kFSub = 0x04,
  kIMul = 0x05,
  kFMul = 0x06,
  kIDiv = 0x07,
  kFDiv = 0x08,
  kIMod = 0x09,
  kNot = 0x0A,
  kINeg = 0x0B,
  kFNeg = 0x0C,
  kAssign = 0x0D,
  kCast = 0x0E,
  kCmpEq = 0x0F,
  kCmpLt = 0x10,
  kCmpLe = 0x11,
  kCmpGt = 0x12,
  kCmpGe = 0x13,
  kJmp = 0x14,
  kJmpT = 0x15,
  kJmpF = 0x16,
  kCallMethod = 0x17,
  kCallParent = 0x18,
  kCallStatic = 0x19,
  kReturn = 0x1A,
  kStrCat = 0x1B,
  kPropGet = 0x1C,
  kPropSet = 0x1D,
  kArrayCreate = 0x1E,
  kArrayLength = 0x1F,
  kArrayGetElement = 0x20,
  kArraySetElement = 0x21,
  kArrayFindElement = 0x22,
  kArrayRFindElement = 0x23,
  // Fallout 4 / 76 extensions. Listed so the decoder consumes their operands
  // correctly even when a Skyrim VM never executes them.
  kIs = 0x24,
  kStructCreate = 0x25,
  kStructGet = 0x26,
  kStructSet = 0x27,
  kArrayFindStruct = 0x28,
  kArrayRFindStruct = 0x29,
  kArrayAdd = 0x2A,
  kArrayInsert = 0x2B,
  kArrayRemoveLast = 0x2C,
  kArrayRemove = 0x2D,
  kArrayClear = 0x2E,
  kArrayGetAllMatchingStructs = 0x2F,
  kInvalid = 0xFF,
};

// Decode metadata for one opcode. fixed_args is how many operands always
// follow; var_args marks the call opcodes, which carry an extra integer count
// followed by that many operands.
struct OpInfo {
  const char* mnemonic;
  u8 fixed_args;
  bool var_args;
};

// Returns metadata for any opcode value. Unknown values map to kInvalid so a
// corrupt or newer stream is rejected rather than misdecoded.
const OpInfo& GetOpInfo(Op op);
inline const OpInfo& GetOpInfo(u8 raw) { return GetOpInfo(static_cast<Op>(raw)); }

}  // namespace rec::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_OPCODE_H_
