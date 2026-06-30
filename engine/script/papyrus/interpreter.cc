#include "script/papyrus/interpreter.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "core/log.h"
#include "script/papyrus/fiber.h"

namespace rec::script::papyrus {
namespace {

std::string Lower(std::string s) {
  std::ranges::transform(s, s.begin(),
                         [](char c) { return static_cast<char>(std::tolower((unsigned char)c)); });
  return s;
}

// Maps a declared type name to the value kind a cast produces. Script types and
// the dynamic "var" type are not primitive coercions and are handled by Cast.
ValueType TargetType(const std::string& type_name) {
  std::string t = Lower(type_name);
  if (t.size() >= 2 && t.compare(t.size() - 2, 2, "[]") == 0) return ValueType::kArray;
  if (t == "int") return ValueType::kInt;
  if (t == "float") return ValueType::kFloat;
  if (t == "bool") return ValueType::kBool;
  if (t == "string") return ValueType::kString;
  if (t == "none") return ValueType::kNone;
  return ValueType::kObject;  // a script type
}

// One running function: its register file (parameters + locals by name), the
// instance it runs on, and the VM it calls out to.
class Frame {
 public:
  Frame(const PexFile& pex, const Object& object, const Function& fn, ObjectRef self,
        std::vector<Value>& args, VmInterface& vm, std::string_view fn_name)
      : pex_(pex), object_(object), fn_(fn), self_(self), vm_(vm), fn_name_(fn_name) {
    for (size_t i = 0; i < fn.params.size(); ++i) {
      const std::string& name = pex.Str(fn.params[i].name);
      decl_types_[name] = pex.Str(fn.params[i].type);
      regs_[name] = i < args.size() ? std::move(args[i]) : Default(fn.params[i].type);
    }
    for (const TypedName& local : fn.locals) {
      const std::string& name = pex.Str(local.name);
      decl_types_[name] = pex.Str(local.type);
      regs_[name] = Default(local.type);
    }
  }

  Value Run();

 private:
  Value Default(StringIndex type_index) {
    switch (TargetType(pex_.Str(type_index))) {
      case ValueType::kInt:
        return Value::Int(0);
      case ValueType::kFloat:
        return Value::Float(0);
      case ValueType::kBool:
        return Value::Bool(false);
      case ValueType::kString:
        return Value::Str("");
      default:
        return Value();  // object/array/none start as None
    }
  }

  std::string DeclType(const std::string& name) {
    auto it = decl_types_.find(name);
    if (it != decl_types_.end()) return it->second;
    for (const MemberVariable& v : object_.variables)
      if (pex_.Str(v.name) == name) return pex_.Str(v.type);
    return "";
  }

  Value ResolveRead(const std::string& name) {
    if (name == "self" || name == "parent") return Value::Object(self_);
    if (name == "::State") return Value::Str(vm_.CurrentState(self_));
    auto it = regs_.find(name);
    if (it != regs_.end()) return it->second;
    if (Value* m = vm_.MemberVar(self_, name)) return *m;
    return Value();
  }

  void ResolveWrite(const std::string& name, Value value) {
    if (name == "::State") {
      vm_.GotoState(self_, value.ToString());
      return;
    }
    if (name == "self" || name == "parent") return;
    auto it = regs_.find(name);
    if (it != regs_.end()) {
      it->second = std::move(value);
      return;
    }
    if (Value* m = vm_.MemberVar(self_, name)) *m = std::move(value);
    // else discard (e.g. ::NoneVar that is not a declared local)
  }

  std::string OperandName(const VariableData& v) const { return pex_.Str(v.string_index); }

  Value ReadOperand(const VariableData& v) {
    switch (v.type) {
      case VariableData::Type::kNone:
        return Value();
      case VariableData::Type::kIdentifier:
        return ResolveRead(pex_.Str(v.string_index));
      case VariableData::Type::kString:
        return Value::Str(pex_.Str(v.string_index));
      case VariableData::Type::kInteger:
        return Value::Int(v.int_value);
      case VariableData::Type::kFloat:
        return Value::Float(v.float_value);
      case VariableData::Type::kBool:
        return Value::Bool(v.bool_value);
    }
    return Value();
  }

  void Write(const VariableData& dest, Value value) {
    ResolveWrite(OperandName(dest), std::move(value));
  }

  Value Cast(const Value& value, const std::string& type_name) {
    std::string t = Lower(type_name);
    if (t.empty() || t == "var") return value;
    switch (TargetType(type_name)) {
      case ValueType::kInt:
        return Value::Int(value.ToInt());
      case ValueType::kFloat:
        return Value::Float(value.ToFloat());
      case ValueType::kBool:
        return Value::Bool(value.ToBool());
      case ValueType::kString:
        return Value::Str(value.ToString());
      case ValueType::kArray:
        return value.type() == ValueType::kArray ? value : Value::Array({});
      case ValueType::kObject:
        if (value.type() == ValueType::kObject && vm_.IsObjectOfType(value.as_object(), type_name))
          return value;
        return Value::Object({});
      default:
        return Value();
    }
  }

  std::vector<Value> CollectArgs(const Instruction& insn) {
    std::vector<Value> out;
    out.reserve(insn.var_args.size());
    for (const VariableData& a : insn.var_args) out.push_back(ReadOperand(a));
    return out;
  }

  // Finds the index of the array element (a struct) whose `member` equals
  // `value`, scanning forward from start or backward. -1 if none.
  i32 FindStruct(ArrayRef array, const std::string& member, const Value& value, i32 start,
                 bool reverse) {
    i32 n = vm_.ArrayLength(array);
    auto matches = [&](i32 i) {
      Value e = vm_.ArrayGet(array, i);
      return e.type() == ValueType::kStruct && vm_.StructGet(e.as_struct(), member).Equals(value);
    };
    if (reverse) {
      for (i32 i = start < 0 ? n - 1 : std::min(start, n - 1); i >= 0; --i)
        if (matches(i)) return i;
    } else {
      for (i32 i = std::max(0, start); i < n; ++i)
        if (matches(i)) return i;
    }
    return -1;
  }

  const PexFile& pex_;
  const Object& object_;
  const Function& fn_;
  ObjectRef self_;
  VmInterface& vm_;
  std::string_view fn_name_;
  std::unordered_map<std::string, Value> regs_;
  std::unordered_map<std::string, std::string> decl_types_;
};

namespace {
bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

// Utility.Wait* suspends the script in the real game; this VM has no latent
// suspension, so a fragment that polls a condition behind Wait() (e.g. "wait
// until the actors are 3D loaded") spins forever. Recognizing the call lets the
// interpreter bail such a loop quickly instead of grinding to the ceiling.
bool IsLatentWait(std::string_view object, std::string_view method) {
  if (!EqualsIgnoreCase(object, "Utility")) return false;
  return EqualsIgnoreCase(method, "Wait") || EqualsIgnoreCase(method, "WaitMenuMode") ||
         EqualsIgnoreCase(method, "WaitGameTime");
}
}  // namespace

Value Frame::Run() {
  // Guest scripts run on their own thread; a runaway loop must not wedge it.
  // The ceiling is far above any real function's instruction count.
  constexpr u64 kMaxInstructions = 4'000'000;
  // A synchronous fragment that busy-waits on a latent Wait() makes no progress;
  // bail after a small number of waits rather than spinning to kMaxInstructions.
  constexpr u32 kMaxLatentWaits = 256;
  const std::vector<Instruction>& code = fn_.code;
  size_t ip = 0;
  u64 executed = 0;
  u32 latent_waits = 0;
  while (ip < code.size()) {
    if (++executed > kMaxInstructions) {
      REC_WARN("papyrus: {}.{} exceeded {} instructions, aborting", pex_.Str(object_.name),
               fn_name_.empty() ? "?" : fn_name_, kMaxInstructions);
      return Value();
    }
    const Instruction& in = code[ip];
    const std::vector<VariableData>& a = in.args;
    size_t next = ip + 1;
    switch (in.op) {
      case Op::kNop:
        break;
      case Op::kIAdd:
        Write(a[0], Value::Int(ReadOperand(a[1]).ToInt() + ReadOperand(a[2]).ToInt()));
        break;
      case Op::kFAdd:
        Write(a[0], Value::Float(ReadOperand(a[1]).ToFloat() + ReadOperand(a[2]).ToFloat()));
        break;
      case Op::kISub:
        Write(a[0], Value::Int(ReadOperand(a[1]).ToInt() - ReadOperand(a[2]).ToInt()));
        break;
      case Op::kFSub:
        Write(a[0], Value::Float(ReadOperand(a[1]).ToFloat() - ReadOperand(a[2]).ToFloat()));
        break;
      case Op::kIMul:
        Write(a[0], Value::Int(ReadOperand(a[1]).ToInt() * ReadOperand(a[2]).ToInt()));
        break;
      case Op::kFMul:
        Write(a[0], Value::Float(ReadOperand(a[1]).ToFloat() * ReadOperand(a[2]).ToFloat()));
        break;
      case Op::kIDiv: {
        i32 d = ReadOperand(a[2]).ToInt();
        Write(a[0], Value::Int(d != 0 ? ReadOperand(a[1]).ToInt() / d : 0));
        break;
      }
      case Op::kFDiv: {
        f32 d = ReadOperand(a[2]).ToFloat();
        Write(a[0], Value::Float(d != 0 ? ReadOperand(a[1]).ToFloat() / d : 0));
        break;
      }
      case Op::kIMod: {
        i32 d = ReadOperand(a[2]).ToInt();
        Write(a[0], Value::Int(d != 0 ? ReadOperand(a[1]).ToInt() % d : 0));
        break;
      }
      case Op::kNot:
        Write(a[0], Value::Bool(!ReadOperand(a[1]).ToBool()));
        break;
      case Op::kINeg:
        Write(a[0], Value::Int(-ReadOperand(a[1]).ToInt()));
        break;
      case Op::kFNeg:
        Write(a[0], Value::Float(-ReadOperand(a[1]).ToFloat()));
        break;
      case Op::kAssign:
        Write(a[0], ReadOperand(a[1]));
        break;
      case Op::kCast:
        Write(a[0], Cast(ReadOperand(a[1]), DeclType(OperandName(a[0]))));
        break;
      case Op::kCmpEq:
        Write(a[0], Value::Bool(ReadOperand(a[1]).Equals(ReadOperand(a[2]))));
        break;
      case Op::kCmpLt:
        Write(a[0], Value::Bool(ReadOperand(a[1]).Compare(ReadOperand(a[2])) < 0));
        break;
      case Op::kCmpLe:
        Write(a[0], Value::Bool(ReadOperand(a[1]).Compare(ReadOperand(a[2])) <= 0));
        break;
      case Op::kCmpGt:
        Write(a[0], Value::Bool(ReadOperand(a[1]).Compare(ReadOperand(a[2])) > 0));
        break;
      case Op::kCmpGe:
        Write(a[0], Value::Bool(ReadOperand(a[1]).Compare(ReadOperand(a[2])) >= 0));
        break;
      case Op::kJmp:
        next = static_cast<size_t>(static_cast<i64>(ip) + ReadOperand(a[0]).ToInt());
        break;
      case Op::kJmpT:
        if (ReadOperand(a[0]).ToBool())
          next = static_cast<size_t>(static_cast<i64>(ip) + ReadOperand(a[1]).ToInt());
        break;
      case Op::kJmpF:
        if (!ReadOperand(a[0]).ToBool())
          next = static_cast<size_t>(static_cast<i64>(ip) + ReadOperand(a[1]).ToInt());
        break;
      case Op::kCallMethod:
        Write(a[2], vm_.CallMethod(ReadOperand(a[1]).as_object(), OperandName(a[0]),
                                   CollectArgs(in)));
        break;
      case Op::kCallParent:
        Write(a[1], vm_.CallParent(self_, OperandName(a[0]), CollectArgs(in)));
        break;
      case Op::kCallStatic: {
        const std::string object = OperandName(a[0]);
        const std::string method = OperandName(a[1]);
        // On a fiber a latent Wait truly suspends and the loop makes real progress
        // between resumes, so it needs no guard. Off a fiber the wait returns at
        // once, so a poll loop would spin: keep bailing that case after a few spins.
        if (IsLatentWait(object, method) && !Fiber::current() &&
            ++latent_waits > kMaxLatentWaits) {
          REC_WARN("papyrus: {}.{} bailed a latent {}.{}() wait loop after {} waits",
                   pex_.Str(object_.name), fn_name_.empty() ? "?" : fn_name_, object, method,
                   kMaxLatentWaits);
          return Value();
        }
        Write(a[2], vm_.CallStatic(object, method, CollectArgs(in)));
        break;
      }
      case Op::kReturn:
        return a.empty() ? Value() : ReadOperand(a[0]);
      case Op::kStrCat:
        Write(a[0], Value::Str(ReadOperand(a[1]).ToString() + ReadOperand(a[2]).ToString()));
        break;
      case Op::kPropGet:
        Write(a[2], vm_.GetProperty(ReadOperand(a[1]).as_object(), OperandName(a[0])));
        break;
      case Op::kPropSet:
        vm_.SetProperty(ReadOperand(a[1]).as_object(), OperandName(a[0]), ReadOperand(a[2]));
        break;
      case Op::kArrayCreate: {
        std::string elem = DeclType(OperandName(a[0]));
        if (elem.size() >= 2 && elem.compare(elem.size() - 2, 2, "[]") == 0)
          elem.resize(elem.size() - 2);
        Write(a[0], Value::Array(vm_.ArrayCreate(elem, ReadOperand(a[1]).ToInt())));
        break;
      }
      case Op::kArrayLength:
        Write(a[0], Value::Int(vm_.ArrayLength(ReadOperand(a[1]).as_array())));
        break;
      case Op::kArrayGetElement:
        Write(a[0], vm_.ArrayGet(ReadOperand(a[1]).as_array(), ReadOperand(a[2]).ToInt()));
        break;
      case Op::kArraySetElement:
        vm_.ArraySet(ReadOperand(a[0]).as_array(), ReadOperand(a[1]).ToInt(), ReadOperand(a[2]));
        break;
      case Op::kArrayFindElement:
        Write(a[1], Value::Int(vm_.ArrayFind(ReadOperand(a[0]).as_array(), ReadOperand(a[2]),
                                             ReadOperand(a[3]).ToInt())));
        break;
      case Op::kArrayRFindElement:
        Write(a[1], Value::Int(vm_.ArrayRFind(ReadOperand(a[0]).as_array(), ReadOperand(a[2]),
                                              ReadOperand(a[3]).ToInt())));
        break;
      // Fallout 4 / 76 opcodes. The same interpreter runs them so a Fallout
      // dialect needs no new VM core, only its own native table.
      case Op::kIs:
        Write(a[0], Value::Bool(vm_.IsObjectOfType(ReadOperand(a[1]).as_object(), OperandName(a[2]))));
        break;
      case Op::kStructCreate:
        Write(a[0], Value::Struct(vm_.StructCreate(DeclType(OperandName(a[0])))));
        break;
      case Op::kStructGet:
        Write(a[0], vm_.StructGet(ReadOperand(a[1]).as_struct(), OperandName(a[2])));
        break;
      case Op::kStructSet:
        vm_.StructSet(ReadOperand(a[0]).as_struct(), OperandName(a[1]), ReadOperand(a[2]));
        break;
      case Op::kArrayFindStruct:
        Write(a[1], Value::Int(FindStruct(ReadOperand(a[0]).as_array(), OperandName(a[2]),
                                          ReadOperand(a[3]), ReadOperand(a[4]).ToInt(), false)));
        break;
      case Op::kArrayRFindStruct:
        Write(a[1], Value::Int(FindStruct(ReadOperand(a[0]).as_array(), OperandName(a[2]),
                                          ReadOperand(a[3]), ReadOperand(a[4]).ToInt(), true)));
        break;
      case Op::kArrayAdd:
        vm_.ArrayAdd(ReadOperand(a[0]).as_array(), ReadOperand(a[1]),
                     a.size() > 2 ? ReadOperand(a[2]).ToInt() : 1);
        break;
      case Op::kArrayInsert:
        vm_.ArrayInsert(ReadOperand(a[0]).as_array(), ReadOperand(a[2]).ToInt(), ReadOperand(a[1]));
        break;
      case Op::kArrayRemoveLast:
        vm_.ArrayRemoveLast(ReadOperand(a[0]).as_array());
        break;
      case Op::kArrayRemove:
        vm_.ArrayRemove(ReadOperand(a[0]).as_array(), ReadOperand(a[1]).ToInt(),
                        a.size() > 2 ? ReadOperand(a[2]).ToInt() : 1);
        break;
      case Op::kArrayClear:
        vm_.ArrayClear(ReadOperand(a[0]).as_array());
        break;
      case Op::kArrayGetAllMatchingStructs: {
        ArrayRef src = ReadOperand(a[0]).as_array();
        std::string member = OperandName(a[2]);
        Value match = ReadOperand(a[3]);
        ArrayRef out = vm_.ArrayCreate("", 0);
        i32 n = vm_.ArrayLength(src);
        for (i32 i = 0; i < n; ++i) {
          Value e = vm_.ArrayGet(src, i);
          if (e.type() == ValueType::kStruct && vm_.StructGet(e.as_struct(), member).Equals(match))
            vm_.ArrayAdd(out, e, 1);
        }
        Write(a[1], Value::Array(out));
        break;
      }
      default:
        REC_WARN("papyrus: unknown opcode 0x{:02x}", static_cast<int>(in.op));
        break;
    }
    ip = next;
  }
  return Value();
}

}  // namespace

Value ExecuteFunction(const PexFile& pex, const Object& object, const Function& fn, ObjectRef self,
                      std::vector<Value> args, VmInterface& vm, std::string_view function_name) {
  Frame frame(pex, object, fn, self, args, vm, function_name);
  return frame.Run();
}

}  // namespace rec::script::papyrus
