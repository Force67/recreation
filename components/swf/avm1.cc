#include "components/swf/avm1.h"

#include <base/memory/move.h>
#include <base/strings/format.h>

#include "components/swf/types.h"

namespace rx::swf {
namespace {

struct NamedOp {
  u8 code;
  const char* name;
};

constexpr NamedOp kOpNames[] = {
    {op::kEnd, "End"},
    {op::kNextFrame, "NextFrame"},
    {op::kPrevFrame, "PrevFrame"},
    {op::kPlay, "Play"},
    {op::kStop, "Stop"},
    {op::kToggleQuality, "ToggleQuality"},
    {op::kStopSounds, "StopSounds"},
    {op::kAdd, "Add"},
    {op::kSubtract, "Subtract"},
    {op::kMultiply, "Multiply"},
    {op::kDivide, "Divide"},
    {op::kEquals, "Equals"},
    {op::kLess, "Less"},
    {op::kAnd, "And"},
    {op::kOr, "Or"},
    {op::kNot, "Not"},
    {op::kStringEquals, "StringEquals"},
    {op::kStringLength, "StringLength"},
    {op::kStringExtract, "StringExtract"},
    {op::kPop, "Pop"},
    {op::kToInteger, "ToInteger"},
    {op::kGetVariable, "GetVariable"},
    {op::kSetVariable, "SetVariable"},
    {op::kSetTarget2, "SetTarget2"},
    {op::kStringAdd, "StringAdd"},
    {op::kGetProperty, "GetProperty"},
    {op::kSetProperty, "SetProperty"},
    {op::kCloneSprite, "CloneSprite"},
    {op::kRemoveSprite, "RemoveSprite"},
    {op::kTrace, "Trace"},
    {op::kStartDrag, "StartDrag"},
    {op::kEndDrag, "EndDrag"},
    {op::kStringLess, "StringLess"},
    {op::kThrow, "Throw"},
    {op::kCastOp, "CastOp"},
    {op::kImplementsOp, "ImplementsOp"},
    {op::kRandomNumber, "RandomNumber"},
    {op::kMbStringLength, "MBStringLength"},
    {op::kCharToAscii, "CharToAscii"},
    {op::kAsciiToChar, "AsciiToChar"},
    {op::kGetTime, "GetTime"},
    {op::kMbStringExtract, "MBStringExtract"},
    {op::kMbCharToAscii, "MBCharToAscii"},
    {op::kMbAsciiToChar, "MBAsciiToChar"},
    {op::kDelete, "Delete"},
    {op::kDelete2, "Delete2"},
    {op::kDefineLocal, "DefineLocal"},
    {op::kCallFunction, "CallFunction"},
    {op::kReturn, "Return"},
    {op::kModulo, "Modulo"},
    {op::kNewObject, "NewObject"},
    {op::kDefineLocal2, "DefineLocal2"},
    {op::kInitArray, "InitArray"},
    {op::kInitObject, "InitObject"},
    {op::kTypeOf, "TypeOf"},
    {op::kTargetPath, "TargetPath"},
    {op::kEnumerate, "Enumerate"},
    {op::kAdd2, "Add2"},
    {op::kLess2, "Less2"},
    {op::kEquals2, "Equals2"},
    {op::kToNumber, "ToNumber"},
    {op::kToString, "ToString"},
    {op::kPushDuplicate, "PushDuplicate"},
    {op::kStackSwap, "StackSwap"},
    {op::kGetMember, "GetMember"},
    {op::kSetMember, "SetMember"},
    {op::kIncrement, "Increment"},
    {op::kDecrement, "Decrement"},
    {op::kCallMethod, "CallMethod"},
    {op::kNewMethod, "NewMethod"},
    {op::kInstanceOf, "InstanceOf"},
    {op::kEnumerate2, "Enumerate2"},
    {op::kBitAnd, "BitAnd"},
    {op::kBitOr, "BitOr"},
    {op::kBitXor, "BitXor"},
    {op::kBitLShift, "BitLShift"},
    {op::kBitRShift, "BitRShift"},
    {op::kBitURShift, "BitURShift"},
    {op::kStrictEquals, "StrictEquals"},
    {op::kGreater, "Greater"},
    {op::kStringGreater, "StringGreater"},
    {op::kExtends, "Extends"},
    {op::kGotoFrame, "GotoFrame"},
    {op::kGetUrl, "GetURL"},
    {op::kStoreRegister, "StoreRegister"},
    {op::kConstantPool, "ConstantPool"},
    {op::kWaitForFrame, "WaitForFrame"},
    {op::kSetTarget, "SetTarget"},
    {op::kGotoLabel, "GotoLabel"},
    {op::kWaitForFrame2, "WaitForFrame2"},
    {op::kDefineFunction2, "DefineFunction2"},
    {op::kTry, "Try"},
    {op::kWith, "With"},
    {op::kPush, "Push"},
    {op::kJump, "Jump"},
    {op::kGetUrl2, "GetURL2"},
    {op::kDefineFunction, "DefineFunction"},
    {op::kIf, "If"},
    {op::kCall, "Call"},
    {op::kGotoFrame2, "GotoFrame2"},
};

void ReadPushValues(Reader& r, Action& action) {
  while (r.ok() && !r.eof()) {
    Value v;
    switch (r.U8()) {
      case 0:
        v.kind = Value::Kind::kString;
        v.text = r.Str();
        break;
      case 1:
        v.kind = Value::Kind::kFloat;
        v.number = r.F32();
        break;
      case 2:
        v.kind = Value::Kind::kNull;
        break;
      case 3:
        v.kind = Value::Kind::kUndefined;
        break;
      case 4:
        v.kind = Value::Kind::kRegister;
        v.index = r.U8();
        break;
      case 5:
        v.kind = Value::Kind::kBool;
        v.boolean = r.U8() != 0;
        break;
      case 6:
        v.kind = Value::Kind::kDouble;
        v.number = r.F64Swapped();
        break;
      case 7:
        v.kind = Value::Kind::kInt;
        v.number = static_cast<f64>(r.I32());
        break;
      case 8:
        v.kind = Value::Kind::kConstant;
        v.index = r.U8();
        break;
      case 9:
        v.kind = Value::Kind::kConstant;
        v.index = r.U16();
        break;
      default:
        return;  // unknown type: the rest of the payload is unreadable
    }
    if (!r.ok())
      return;
    action.values.push_back(base::move(v));
  }
}

void ReadPayload(u8 code, ByteSpan payload, Action& action) {
  Reader r(payload);
  switch (code) {
    case op::kGotoFrame:
      action.word_arg = r.U16();
      break;
    case op::kGetUrl:
      action.name = r.Str();
      action.secondary = r.Str();
      break;
    case op::kStoreRegister:
      action.byte_arg = r.U8();
      break;
    case op::kConstantPool: {
      const u16 count = r.U16();
      for (u16 i = 0; i < count && r.ok(); ++i)
        action.strings.push_back(r.Str());
      break;
    }
    case op::kWaitForFrame:
      action.word_arg = r.U16();
      action.byte_arg = r.U8();
      break;
    case op::kSetTarget:
    case op::kGotoLabel:
      action.name = r.Str();
      break;
    case op::kWaitForFrame2:
    case op::kGetUrl2:
    case op::kGotoFrame2:
      action.byte_arg = r.U8();
      break;
    case op::kDefineFunction: {
      action.name = r.Str();
      action.param_count = r.U16();
      for (u16 i = 0; i < action.param_count && r.ok(); ++i)
        action.strings.push_back(r.Str());
      action.body_size = r.U16();
      break;
    }
    case op::kDefineFunction2: {
      action.name = r.Str();
      action.param_count = r.U16();
      action.register_count = r.U8();
      action.function_flags = r.U16();
      for (u16 i = 0; i < action.param_count && r.ok(); ++i) {
        action.param_registers.push_back(r.U8());
        action.strings.push_back(r.Str());
      }
      action.body_size = r.U16();
      break;
    }
    case op::kTry: {
      const u8 flags = r.U8();
      action.byte_arg = flags;
      action.body_size = r.U16();  // try block size
      action.word_arg = r.U16();   // catch block size
      action.param_count = r.U16();  // finally block size
      if (flags & 0x04)
        action.register_count = r.U8();
      else
        action.name = r.Str();
      break;
    }
    case op::kWith:
      action.body_size = r.U16();
      break;
    case op::kPush:
      ReadPushValues(r, action);
      break;
    case op::kJump:
    case op::kIf:
      action.jump = r.I16();
      break;
    default:
      break;
  }
}

void FormatValue(const Value& v,
                 const base::Vector<base::String>& pool,
                 base::String& out) {
  switch (v.kind) {
    case Value::Kind::kString:
      out += '"';
      out += v.text;
      out += '"';
      break;
    case Value::Kind::kFloat:
    case Value::Kind::kDouble:
    case Value::Kind::kInt:
      out += base::Format("{}", v.number);
      break;
    case Value::Kind::kNull:
      out += "null";
      break;
    case Value::Kind::kUndefined:
      out += "undefined";
      break;
    case Value::Kind::kRegister:
      out += base::Format("r{}", v.index);
      break;
    case Value::Kind::kBool:
      out += v.boolean ? "true" : "false";
      break;
    case Value::Kind::kConstant:
      if (v.index < pool.size()) {
        out += '"';
        out += pool[v.index];
        out += '"';
      } else {
        out += base::Format("const{}", v.index);
      }
      break;
  }
}

}  // namespace

base::StringRef OpName(u8 code) {
  for (const NamedOp& o : kOpNames)
    if (o.code == code)
      return o.name;
  return "Unknown";
}

base::StringRef ClipEventName(u32 bit) {
  static const char* const kNames[] = {
      "load",      "enterFrame", "unload",         "mouseMove", "mouseDown",
      "mouseUp",   "keyDown",    "keyUp",          "data",      "initialize",
      "press",     "release",    "releaseOutside", "rollOver",  "rollOut",
      "dragOver",  "dragOut",    "keyPress",       "construct"};
  if (bit >= sizeof(kNames) / sizeof(kNames[0]))
    return base::StringRef();
  return kNames[bit];
}

base::Vector<Action> Disassemble(ByteSpan code) {
  base::Vector<Action> out;
  Reader r(code);
  while (r.ok() && !r.eof()) {
    Action action;
    action.offset = static_cast<u32>(r.pos());
    action.code = r.U8();
    if (!r.ok())
      break;
    if (action.code >= 0x80) {
      const u16 length = r.U16();
      const ByteSpan payload = r.Bytes(length);
      if (!r.ok())
        break;
      ReadPayload(action.code, payload, action);
    }
    action.end = static_cast<u32>(r.pos());
    out.push_back(base::move(action));
  }
  return out;
}

base::String FormatAction(const Action& action, const base::Vector<base::String>& pool) {
  base::String out = base::Format("{:04x}  {}", action.offset, OpName(action.code));
  switch (action.code) {
    case op::kPush: {
      out += "  ";
      for (mem_size i = 0; i < action.values.size(); ++i) {
        if (i)
          out += ", ";
        FormatValue(action.values[i], pool, out);
      }
      break;
    }
    case op::kConstantPool:
      out += base::Format("  {} entries", action.strings.size());
      break;
    case op::kJump:
    case op::kIf:
      out += base::Format("  -> {:04x}",
                          static_cast<u32>(static_cast<i32>(action.end) + action.jump));
      break;
    case op::kDefineFunction:
    case op::kDefineFunction2: {
      out += "  ";
      out += action.name.empty() ? base::String("<anonymous>") : action.name;
      out += '(';
      for (mem_size i = 0; i < action.strings.size(); ++i) {
        if (i)
          out += ", ";
        out += action.strings[i];
      }
      out += base::Format(") body={} bytes", action.body_size);
      break;
    }
    case op::kStoreRegister:
      out += base::Format("  r{}", action.byte_arg);
      break;
    case op::kGetUrl:
      out += base::Format("  \"{}\" -> \"{}\"", action.name, action.secondary);
      break;
    case op::kSetTarget:
    case op::kGotoLabel:
      out += base::Format("  \"{}\"", action.name);
      break;
    case op::kGotoFrame:
      out += base::Format("  {}", action.word_arg);
      break;
    case op::kWith:
      out += base::Format("  body={} bytes", action.body_size);
      break;
    default:
      break;
  }
  return out;
}

}  // namespace rx::swf
