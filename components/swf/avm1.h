#ifndef RECREATION_SWF_AVM1_H_
#define RECREATION_SWF_AVM1_H_

#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace rx::swf {

// AVM1 (ActionScript 2) opcodes. Bethesda's Scaleform menus are built entirely
// from this instruction set: there is not one DoABC tag in any shipped Skyrim,
// Fallout or Starfield interface archive.
namespace op {
constexpr u8 kEnd = 0x00;
constexpr u8 kNextFrame = 0x04;
constexpr u8 kPrevFrame = 0x05;
constexpr u8 kPlay = 0x06;
constexpr u8 kStop = 0x07;
constexpr u8 kToggleQuality = 0x08;
constexpr u8 kStopSounds = 0x09;
constexpr u8 kAdd = 0x0a;
constexpr u8 kSubtract = 0x0b;
constexpr u8 kMultiply = 0x0c;
constexpr u8 kDivide = 0x0d;
constexpr u8 kEquals = 0x0e;
constexpr u8 kLess = 0x0f;
constexpr u8 kAnd = 0x10;
constexpr u8 kOr = 0x11;
constexpr u8 kNot = 0x12;
constexpr u8 kStringEquals = 0x13;
constexpr u8 kStringLength = 0x14;
constexpr u8 kStringExtract = 0x15;
constexpr u8 kPop = 0x17;
constexpr u8 kToInteger = 0x18;
constexpr u8 kGetVariable = 0x1c;
constexpr u8 kSetVariable = 0x1d;
constexpr u8 kSetTarget2 = 0x20;
constexpr u8 kStringAdd = 0x21;
constexpr u8 kGetProperty = 0x22;
constexpr u8 kSetProperty = 0x23;
constexpr u8 kCloneSprite = 0x24;
constexpr u8 kRemoveSprite = 0x25;
constexpr u8 kTrace = 0x26;
constexpr u8 kStartDrag = 0x27;
constexpr u8 kEndDrag = 0x28;
constexpr u8 kStringLess = 0x29;
constexpr u8 kThrow = 0x2a;
constexpr u8 kCastOp = 0x2b;
constexpr u8 kImplementsOp = 0x2c;
constexpr u8 kRandomNumber = 0x30;
constexpr u8 kMbStringLength = 0x31;
constexpr u8 kCharToAscii = 0x32;
constexpr u8 kAsciiToChar = 0x33;
constexpr u8 kGetTime = 0x34;
constexpr u8 kMbStringExtract = 0x35;
constexpr u8 kMbCharToAscii = 0x36;
constexpr u8 kMbAsciiToChar = 0x37;
constexpr u8 kDelete = 0x3a;
constexpr u8 kDelete2 = 0x3b;
constexpr u8 kDefineLocal = 0x3c;
constexpr u8 kCallFunction = 0x3d;
constexpr u8 kReturn = 0x3e;
constexpr u8 kModulo = 0x3f;
constexpr u8 kNewObject = 0x40;
constexpr u8 kDefineLocal2 = 0x41;
constexpr u8 kInitArray = 0x42;
constexpr u8 kInitObject = 0x43;
constexpr u8 kTypeOf = 0x44;
constexpr u8 kTargetPath = 0x45;
constexpr u8 kEnumerate = 0x46;
constexpr u8 kAdd2 = 0x47;
constexpr u8 kLess2 = 0x48;
constexpr u8 kEquals2 = 0x49;
constexpr u8 kToNumber = 0x4a;
constexpr u8 kToString = 0x4b;
constexpr u8 kPushDuplicate = 0x4c;
constexpr u8 kStackSwap = 0x4d;
constexpr u8 kGetMember = 0x4e;
constexpr u8 kSetMember = 0x4f;
constexpr u8 kIncrement = 0x50;
constexpr u8 kDecrement = 0x51;
constexpr u8 kCallMethod = 0x52;
constexpr u8 kNewMethod = 0x53;
constexpr u8 kInstanceOf = 0x54;
constexpr u8 kEnumerate2 = 0x55;
constexpr u8 kBitAnd = 0x60;
constexpr u8 kBitOr = 0x61;
constexpr u8 kBitXor = 0x62;
constexpr u8 kBitLShift = 0x63;
constexpr u8 kBitRShift = 0x64;
constexpr u8 kBitURShift = 0x65;
constexpr u8 kStrictEquals = 0x66;
constexpr u8 kGreater = 0x67;
constexpr u8 kStringGreater = 0x68;
constexpr u8 kExtends = 0x69;
constexpr u8 kGotoFrame = 0x81;
constexpr u8 kGetUrl = 0x83;
constexpr u8 kStoreRegister = 0x87;
constexpr u8 kConstantPool = 0x88;
constexpr u8 kWaitForFrame = 0x8a;
constexpr u8 kSetTarget = 0x8b;
constexpr u8 kGotoLabel = 0x8c;
constexpr u8 kWaitForFrame2 = 0x8d;
constexpr u8 kDefineFunction2 = 0x8e;
constexpr u8 kTry = 0x8f;
constexpr u8 kWith = 0x94;
constexpr u8 kPush = 0x96;
constexpr u8 kJump = 0x99;
constexpr u8 kGetUrl2 = 0x9a;
constexpr u8 kDefineFunction = 0x9b;
constexpr u8 kIf = 0x9d;
constexpr u8 kCall = 0x9e;
constexpr u8 kGotoFrame2 = 0x9f;
}  // namespace op

base::StringRef OpName(u8 code);

// The on(...) / onClipEvent(...) name for one bit of a clip-event flag word,
// in the order the flags are packed. Empty for bits the spec reserves.
base::StringRef ClipEventName(u32 bit);

// One operand of a Push action.
struct Value {
  enum class Kind : u8 {
    kString,
    kFloat,
    kNull,
    kUndefined,
    kRegister,
    kBool,
    kDouble,
    kInt,
    kConstant,
  };

  Kind kind = Kind::kUndefined;
  base::String text;   // kString
  f64 number = 0;      // kFloat / kDouble / kInt
  u32 index = 0;       // kRegister / kConstant
  bool boolean = false;
};

// DefineFunction2 preload/suppress flags, in the order the spec lists them.
namespace fn_flags {
constexpr u16 kPreloadThis = 0x0001;
constexpr u16 kSuppressThis = 0x0002;
constexpr u16 kPreloadArguments = 0x0004;
constexpr u16 kSuppressArguments = 0x0008;
constexpr u16 kPreloadSuper = 0x0010;
constexpr u16 kSuppressSuper = 0x0020;
constexpr u16 kPreloadRoot = 0x0040;
constexpr u16 kPreloadParent = 0x0080;
constexpr u16 kPreloadGlobal = 0x0100;
}  // namespace fn_flags

// A decoded action. `offset` and `end` are byte positions inside the script the
// action came from, which is what the jump operands are relative to.
struct Action {
  u8 code = 0;
  u32 offset = 0;
  u32 end = 0;

  base::Vector<Value> values;         // Push
  base::Vector<base::String> strings; // ConstantPool entries, function parameters
  base::Vector<u8> param_registers;   // DefineFunction2, parallel to `strings`
  base::String name;                  // function name / target / label / url
  base::String secondary;             // GetURL target window
  i32 jump = 0;                       // Jump / If, relative to `end`
  u32 body_size = 0;                  // DefineFunction/2, With, Try body length
  u16 param_count = 0;
  u16 function_flags = 0;
  u8 register_count = 0;
  u8 byte_arg = 0;  // StoreRegister index, GetURL2 method, GotoFrame2 flags
  u16 word_arg = 0; // GotoFrame frame, WaitForFrame frame
};

// Linear decode of an action block. Stops at the End action or the end of the
// buffer; a truncated action ends the list rather than failing, so a partially
// readable script still yields everything before the damage.
base::Vector<Action> Disassemble(ByteSpan code);

// Human-readable one-line form, e.g. `0042  Push  "onLoad", 3`. `pool` resolves
// constant references; pass the pool in force at that point in the script.
base::String FormatAction(const Action& action, const base::Vector<base::String>& pool);

}  // namespace rx::swf

#endif  // RECREATION_SWF_AVM1_H_
