#include "components/swf/vm.h"

#include <base/algorithm.h>
#include <base/containers/pair.h>
#include <base/memory/move.h>
#include <base/strings/format.h>

#include <cmath>

namespace rx::swf {
namespace {

// A run is bounded so a script that loops forever stops instead of hanging the
// caller. Menus settle in well under this; the start menu's whole init is a few
// hundred thousand instructions.
constexpr u64 kMaxSteps = 40'000'000;
constexpr u32 kMaxDepth = 64;

// The AVM1 indexed property table, in opcode order. GetProperty/SetProperty
// address a clip's properties by number; they become ordinary member names so a
// host native can implement them alongside everything else.
const char* const kProperties[] = {
    "_x",            "_y",       "_xscale",   "_yscale",    "_currentframe",
    "_totalframes",  "_alpha",   "_visible",  "_width",     "_height",
    "_rotation",     "_target",  "_framesloaded", "_name",  "_droptarget",
    "_url",          "_highquality", "_focusrect", "_soundbuftime", "_quality",
    "_xmouse",       "_ymouse",
};

bool IsNumericString(base::StringRef s, f64& out) {
  if (s.empty())
    return false;
  base::String copy(s);
  char* end = nullptr;
  const f64 value = std::strtod(copy.c_str(), &end);
  if (!end || *end != '\0')
    return false;
  out = value;
  return true;
}

}  // namespace

AsValue AsValue::Null() {
  AsValue v;
  v.type_ = Type::kNull;
  return v;
}
AsValue AsValue::Bool(bool b) {
  AsValue v;
  v.type_ = Type::kBool;
  v.bool_ = b;
  return v;
}
AsValue AsValue::Number(f64 n) {
  AsValue v;
  v.type_ = Type::kNumber;
  v.number_ = n;
  return v;
}
AsValue AsValue::Str(base::StringRef s) {
  AsValue v;
  v.type_ = Type::kString;
  v.string_ = base::String(s);
  return v;
}
AsValue AsValue::Obj(u32 index) {
  AsValue v;
  v.type_ = Type::kObject;
  v.object_ = index;
  return v;
}

// One activation: the operand stack, registers, and the places a bare name is
// looked up in.
struct Vm::Frame {
  AsValue self;
  base::Vector<AsValue> stack;
  base::Vector<AsValue> registers;
  base::Vector<u32> with_chain;
  base::Vector<base::String> pool;
  u32 locals = 0;  // object holding this activation's `var`s
  u32 scope = 0;   // the scope a closure captured
  AsValue result;
  bool returned = false;
  // Set for a script that runs AS a timeline rather than as a function body: a
  // frame script or a clip-event handler. A bare assignment there names a
  // property of that timeline, not a global.
  bool timeline = false;

  AsValue Pop() {
    if (stack.empty())
      return AsValue::Undefined();  // an underflowing script gets undefined, not a crash
    AsValue v = base::move(stack[stack.size() - 1]);
    stack.pop_back();
    return v;
  }
  void Push(AsValue v) { stack.push_back(base::move(v)); }
};

Vm::Vm() {
  objects_.push_back(AsObject{});  // index 0 is the null object
  global_ = NewObject();
  InstallStandardLibrary();
}

u32 Vm::NewObject(u32 prototype) {
  AsObject object;
  object.prototype = prototype;
  objects_.push_back(base::move(object));
  return static_cast<u32>(objects_.size() - 1);
}

u32 Vm::NewArray() {
  const u32 index = NewObject(array_prototype_);
  objects_[index].is_array = true;
  objects_[index].props[base::String("length")] = AsValue::Number(0);
  return index;
}

u32 Vm::NewNative(NativeFn fn) {
  const u32 index = NewObject(function_prototype_);
  objects_[index].is_function = true;
  objects_[index].native = fn;
  return index;
}

u32 Vm::AddScript(ByteSpan code) {
  AsScript script;
  script.actions = Disassemble(code);
  scripts_.push_back(base::move(script));
  return static_cast<u32>(scripts_.size() - 1);
}

bool Vm::ToBool(const AsValue& v) const {
  switch (v.type()) {
    case AsValue::Type::kUndefined:
    case AsValue::Type::kNull:
      return false;
    case AsValue::Type::kBool:
      return v.raw_bool();
    case AsValue::Type::kNumber:
      return v.raw_number() != 0 && !std::isnan(v.raw_number());
    case AsValue::Type::kString:
      return !v.string().empty() && v.string() != "0";
    case AsValue::Type::kObject:
      return v.object() != 0;
  }
  return false;
}

f64 Vm::ToNumber(const AsValue& v) const {
  switch (v.type()) {
    case AsValue::Type::kBool:
      return v.raw_bool() ? 1 : 0;
    case AsValue::Type::kNumber:
      return v.raw_number();
    case AsValue::Type::kString: {
      f64 out = 0;
      if (v.string().empty())
        return 0;
      return IsNumericString(v.string(), out) ? out : std::nan("");
    }
    case AsValue::Type::kUndefined:
      return std::nan("");
    case AsValue::Type::kNull:
      return 0;
    case AsValue::Type::kObject:
      return std::nan("");
  }
  return 0;
}

base::String Vm::ToString(const AsValue& v) {
  switch (v.type()) {
    case AsValue::Type::kUndefined:
      return base::String("undefined");
    case AsValue::Type::kNull:
      return base::String("null");
    case AsValue::Type::kBool:
      return base::String(v.raw_bool() ? "true" : "false");
    case AsValue::Type::kNumber: {
      const f64 n = v.raw_number();
      if (std::isnan(n))
        return base::String("NaN");
      if (std::isinf(n))
        return base::String(n < 0 ? "-Infinity" : "Infinity");
      if (n == static_cast<f64>(static_cast<i64>(n)))
        return base::Format("{}", static_cast<i64>(n));
      return base::Format("{}", n);
    }
    case AsValue::Type::kString:
      return v.string();
    case AsValue::Type::kObject: {
      if (!Valid(v.object()))
        return base::String("null");
      const AsObject& o = objects_[v.object()];
      if (o.is_array) {
        base::String out;
        const AsValue* length = o.props.find(base::String("length"));
        const i64 n = length ? static_cast<i64>(ToNumber(*length)) : 0;
        for (i64 i = 0; i < n; ++i) {
          if (i)
            out += ',';
          const AsValue* item = objects_[v.object()].props.find(base::Format("{}", i));
          if (item)
            out += ToString(*item);
        }
        return out;
      }
      return base::String(o.is_function ? "[type Function]" : "[object Object]");
    }
  }
  return {};
}

AsValue Vm::GetMember(const AsValue& target, base::StringRef name) {
  if (target.is_string()) {
    if (name == "length")
      return AsValue::Number(static_cast<f64>(target.string().size()));
    // Methods come off the shared String prototype, with the string itself
    // arriving as `this` when the call is made.
    if (Valid(string_prototype_))
      return GetMember(AsValue::Obj(string_prototype_), name);
    return AsValue::Undefined();
  }
  if (!target.is_object() || !Valid(target.object()))
    return AsValue::Undefined();
  const base::String key(name);
  u32 current = target.object();
  for (u32 guard = 0; guard < 64 && Valid(current); ++guard) {
    if (const AsValue* found = objects_[current].props.find(key))
      return *found;
    // An accessor installed by addProperty runs with `this` bound to the object
    // the read started from, not to the prototype that carries it.
    if (const AsAccessor* accessor = objects_[current].accessors.find(key)) {
      const AsValue getter = accessor->getter;
      if (getter.is_object())
        return CallInternal(getter, target, base::Vector<AsValue>(), depth_ + 1);
      return AsValue::Undefined();
    }
    current = objects_[current].prototype;
  }
  return AsValue::Undefined();
}

void Vm::SetMember(const AsValue& target, base::StringRef name, const AsValue& value) {
  if (!target.is_object() || !Valid(target.object()))
    return;
  const base::String key(name);
  for (u32 current = target.object(), guard = 0; guard < 64 && Valid(current); ++guard) {
    if (objects_[current].props.find(key))
      break;  // an own slot shadows an inherited accessor
    if (const AsAccessor* accessor = objects_[current].accessors.find(key)) {
      const AsValue setter = accessor->setter;
      if (setter.is_object()) {
        base::Vector<AsValue> args;
        args.push_back(value);
        CallInternal(setter, target, args, depth_ + 1);
      }
      return;
    }
    current = objects_[current].prototype;
  }

  AsObject& object = objects_[target.object()];
  if (!object.props.find(key))
    object.order.push_back(key);
  object.props[key] = value;
  // An array tracks its own length as the language does, so `push` and indexed
  // writes agree without the host having to intervene.
  if (object.is_array) {
    f64 index = 0;
    if (IsNumericString(name, index)) {
      const AsValue* length = object.props.find(base::String("length"));
      const f64 have = length ? ToNumber(*length) : 0;
      if (index + 1 > have)
        object.props[base::String("length")] = AsValue::Number(index + 1);
    }
  }
}

u32 Vm::IndexOfOffset(const AsScript& script, u32 first, u32 count, u32 offset) const {
  for (u32 i = first; i < first + count && i < script.actions.size(); ++i)
    if (script.actions[i].offset == offset)
      return i;
  return first + count;
}

AsValue Vm::ResolveVariable(Frame& frame, base::StringRef name) {
  // GetVariable takes a path, not a bare name: the compiler emits
  // `Push "Shared.CenteredScrollingList"; GetVariable` rather than a chain of
  // GetMembers. Resolve the head the normal way and walk the rest.
  for (mem_size i = 0; i < name.size(); ++i) {
    if (name[i] != '.')
      continue;
    AsValue current = ResolveVariable(frame, name.subslice(0, i));
    mem_size start = i + 1;
    for (mem_size j = start; j <= name.size(); ++j) {
      if (j != name.size() && name[j] != '.')
        continue;
      current = GetMember(current, name.subslice(start, j - start));
      start = j + 1;
    }
    return current;
  }
  const base::String key(name);
  for (mem_size i = frame.with_chain.size(); i-- > 0;) {
    const AsValue scope = AsValue::Obj(frame.with_chain[i]);
    if (Valid(frame.with_chain[i]) && objects_[frame.with_chain[i]].props.find(key))
      return GetMember(scope, name);
  }
  if (Valid(frame.locals) && objects_[frame.locals].props.find(key))
    return objects_[frame.locals].props[key];
  if (Valid(frame.scope)) {
    u32 current = frame.scope;
    for (u32 guard = 0; guard < 64 && Valid(current); ++guard) {
      if (const AsValue* found = objects_[current].props.find(key))
        return *found;
      current = objects_[current].prototype;
    }
  }
  if (name == "this")
    return frame.self;
  if (name == "_global")
    return AsValue::Obj(global_);
  if (name == "_root" || name == "_level0")
    return root_;
  if (frame.self.is_object()) {
    const AsValue member = GetMember(frame.self, name);
    if (!member.is_undefined())
      return member;
  }
  return GetMember(AsValue::Obj(global_), name);
}

void Vm::AssignVariable(Frame& frame, base::StringRef name, const AsValue& value) {
  // SetVariable takes a path too; everything but the last segment names the
  // object the assignment lands on.
  for (mem_size i = name.size(); i-- > 0;) {
    if (name[i] != '.')
      continue;
    const AsValue target = ResolveVariable(frame, name.subslice(0, i));
    SetMember(target, name.subslice(i + 1, name.size() - i - 1), value);
    return;
  }
  const base::String key(name);
  for (mem_size i = frame.with_chain.size(); i-- > 0;) {
    if (Valid(frame.with_chain[i]) && objects_[frame.with_chain[i]].props.find(key)) {
      SetMember(AsValue::Obj(frame.with_chain[i]), name, value);
      return;
    }
  }
  if (Valid(frame.locals) && objects_[frame.locals].props.find(key)) {
    objects_[frame.locals].props[key] = value;
    return;
  }
  if (frame.self.is_object() && Valid(frame.self.object()) &&
      objects_[frame.self.object()].props.find(key)) {
    SetMember(frame.self, name, value);
    return;
  }
  // A bare name assigned on a timeline is that timeline's, which is how a
  // component's authored parameters (a tab's labelID, a list's row count) reach
  // the clip they were placed on. Everywhere else it is a global.
  if (frame.timeline && frame.self.is_object() && Valid(frame.self.object()) &&
      objects_[frame.self.object()].is_movie_clip) {
    SetMember(frame.self, name, value);
    return;
  }
  SetMember(AsValue::Obj(global_), name, value);
}

void Vm::Run(u32 script, const AsValue& self) {
  if (script >= scripts_.size())
    return;
  Frame frame;
  frame.self = self;
  frame.timeline = true;
  frame.locals = NewObject();
  frame.registers.resize(256);
  Execute(script, 0, static_cast<u32>(scripts_[script].actions.size()), frame);
}

AsValue Vm::Call(const AsValue& function, const AsValue& self,
                 const base::Vector<AsValue>& args) {
  return CallInternal(function, self, args, 0);
}

AsValue Vm::CallInternal(const AsValue& function, const AsValue& self,
                         const base::Vector<AsValue>& args, u32 depth) {
  if (!function.is_object() || !Valid(function.object()) || depth > kMaxDepth)
    return AsValue::Undefined();
  const AsObject& fn = objects_[function.object()];
  if (!fn.is_function)
    return AsValue::Undefined();
  if (fn.native)
    return fn.native(*this, self, args);

  const AsFunctionBody body = fn.body;  // copied: objects_ can reallocate
  const u32 scope = fn.scope;
  if (body.script >= scripts_.size())
    return AsValue::Undefined();

  Frame frame;
  frame.self = self;
  frame.locals = NewObject();
  frame.scope = scope;
  frame.registers.resize(base::Max<mem_size>(256, body.register_count + 1u));
  frame.pool = body.pool;

  // DefineFunction2 preloads the implicit values into low registers and can
  // suppress the matching locals; DefineFunction (flags 0) uses locals only.
  u8 next_register = 1;
  const bool is_v2 = body.register_count != 0 || body.flags != 0;
  if (is_v2) {
    if (body.flags & fn_flags::kPreloadThis)
      frame.registers[next_register++] = self;
    if (body.flags & fn_flags::kPreloadArguments) {
      const u32 arguments = NewArray();
      for (mem_size i = 0; i < args.size(); ++i)
        SetMember(AsValue::Obj(arguments), base::Format("{}", i), args[i]);
      SetMember(AsValue::Obj(arguments), "length",
                AsValue::Number(static_cast<f64>(args.size())));
      frame.registers[next_register++] = AsValue::Obj(arguments);
    }
    if (body.flags & fn_flags::kPreloadSuper)
      frame.registers[next_register++] = MakeSuper(self);
    if (body.flags & fn_flags::kPreloadRoot)
      frame.registers[next_register++] = root_;
    if (body.flags & fn_flags::kPreloadParent)
      frame.registers[next_register++] = GetMember(self, "_parent");
    if (body.flags & fn_flags::kPreloadGlobal)
      frame.registers[next_register++] = AsValue::Obj(global_);
  }
  if (!(body.flags & fn_flags::kSuppressThis))
    SetMember(AsValue::Obj(frame.locals), "this", self);
  if (!(body.flags & fn_flags::kSuppressArguments)) {
    const u32 arguments = NewArray();
    for (mem_size i = 0; i < args.size(); ++i)
      SetMember(AsValue::Obj(arguments), base::Format("{}", i), args[i]);
    SetMember(AsValue::Obj(arguments), "length",
              AsValue::Number(static_cast<f64>(args.size())));
    SetMember(AsValue::Obj(frame.locals), "arguments", AsValue::Obj(arguments));
  }

  for (mem_size i = 0; i < body.params.size(); ++i) {
    const AsValue value = i < args.size() ? args[i] : AsValue::Undefined();
    const u8 reg = i < body.param_registers.size() ? body.param_registers[i] : 0;
    if (reg != 0 && reg < frame.registers.size())
      frame.registers[reg] = value;
    else
      SetMember(AsValue::Obj(frame.locals), body.params[i], value);
  }

  ++depth_;
  Execute(body.script, body.first, body.count, frame);
  --depth_;

  // `SomeClass(value)` is a cast in ActionScript 2, not a construction: it
  // yields the value when it is already of that class and null otherwise. The
  // compiler emits it as a plain call on the class, so without this the class's
  // constructor body runs against nothing and the cast comes back undefined.
  // The menus lean on it - `ButtonGroup(this.QuestsTab.group)` is how the
  // journal gets its tab strip, and a NaN there took the whole page with it.
  if (frame.result.is_undefined() && self.is_undefined() && args.size() == 1 &&
      args[0].is_object() && Valid(args[0].object())) {
    const AsValue prototype = GetMember(function, "prototype");
    if (prototype.is_object()) {
      for (u32 current = objects_[args[0].object()].prototype, guard = 0;
           guard < 64 && Valid(current); ++guard) {
        if (current == prototype.object())
          return args[0];
        current = objects_[current].prototype;
      }
    }
  }
  return frame.result;
}

// `super` has to answer to both uses the language makes of it: `super()` calls
// the base constructor, and `super.method()` reaches a base method. So it is an
// object whose prototype is the base prototype (giving the methods) carrying a
// copy of the base constructor's body (making it callable).
AsValue Vm::MakeSuper(const AsValue& self) {
  if (!self.is_object() || !Valid(self.object()))
    return AsValue::Undefined();
  const u32 own_proto = objects_[self.object()].prototype;
  if (!Valid(own_proto))
    return AsValue::Undefined();
  const AsValue base_ctor = GetMember(AsValue::Obj(own_proto), "__constructor__");
  const u32 base_proto = objects_[own_proto].prototype;
  const u32 super = NewObject(base_proto);
  if (base_ctor.is_object() && Valid(base_ctor.object())) {
    const AsObject& ctor = objects_[base_ctor.object()];
    objects_[super].is_function = ctor.is_function;
    objects_[super].native = ctor.native;
    objects_[super].body = ctor.body;
    objects_[super].scope = ctor.scope;
  }
  return AsValue::Obj(super);
}

void Vm::Execute(u32 script_index, u32 first, u32 count, Frame& frame) {
  if (script_index >= scripts_.size())
    return;
  const u32 last = first + count;
  u32 i = first;
  while (i < last && i < scripts_[script_index].actions.size()) {
    if (++steps_ > kMaxSteps) {
      exhausted_ = true;
      return;
    }
    if (frame.returned)
      return;
    // The action list can be reallocated only by AddScript, which never runs
    // during execution, so a reference is safe for the body of one step.
    const Action& action = scripts_[script_index].actions[i];
    const u8 code = action.code;
    u32 next = i + 1;

    switch (code) {
      case op::kEnd:
        return;

      case op::kConstantPool:
        frame.pool = action.strings;
        break;

      case op::kPush:
        for (const Value& value : action.values) {
          switch (value.kind) {
            case Value::Kind::kString:
              frame.Push(AsValue::Str(value.text));
              break;
            case Value::Kind::kFloat:
            case Value::Kind::kDouble:
            case Value::Kind::kInt:
              frame.Push(AsValue::Number(value.number));
              break;
            case Value::Kind::kNull:
              frame.Push(AsValue::Null());
              break;
            case Value::Kind::kUndefined:
              frame.Push(AsValue::Undefined());
              break;
            case Value::Kind::kBool:
              frame.Push(AsValue::Bool(value.boolean));
              break;
            case Value::Kind::kRegister:
              frame.Push(value.index < frame.registers.size() ? frame.registers[value.index]
                                                              : AsValue::Undefined());
              break;
            case Value::Kind::kConstant:
              frame.Push(value.index < frame.pool.size()
                             ? AsValue::Str(frame.pool[value.index])
                             : AsValue::Undefined());
              break;
          }
        }
        break;

      case op::kPop:
        frame.Pop();
        break;
      case op::kPushDuplicate:
        if (!frame.stack.empty())
          frame.Push(frame.stack[frame.stack.size() - 1]);
        break;
      case op::kStackSwap:
        if (frame.stack.size() >= 2) {
          const mem_size n = frame.stack.size();
          AsValue tmp = base::move(frame.stack[n - 1]);
          frame.stack[n - 1] = base::move(frame.stack[n - 2]);
          frame.stack[n - 2] = base::move(tmp);
        }
        break;
      case op::kStoreRegister:
        if (!frame.stack.empty() && action.byte_arg < frame.registers.size())
          frame.registers[action.byte_arg] = frame.stack[frame.stack.size() - 1];
        break;

      case op::kGetVariable: {
        const AsValue name = frame.Pop();
        frame.Push(ResolveVariable(frame, ToString(name)));
        break;
      }
      case op::kSetVariable: {
        const AsValue value = frame.Pop();
        const AsValue name = frame.Pop();
        AssignVariable(frame, ToString(name), value);
        break;
      }
      case op::kDefineLocal: {
        const AsValue value = frame.Pop();
        const AsValue name = frame.Pop();
        SetMember(AsValue::Obj(frame.locals), ToString(name), value);
        break;
      }
      case op::kDefineLocal2: {
        const AsValue name = frame.Pop();
        const base::String key = ToString(name);
        if (!objects_[frame.locals].props.find(key))
          SetMember(AsValue::Obj(frame.locals), key, AsValue::Undefined());
        break;
      }

      case op::kGetMember: {
        const AsValue name = frame.Pop();
        const AsValue target = frame.Pop();
        frame.Push(GetMember(target, ToString(name)));
        break;
      }
      case op::kSetMember: {
        const AsValue value = frame.Pop();
        const AsValue name = frame.Pop();
        const AsValue target = frame.Pop();
        SetMember(target, ToString(name), value);
        break;
      }
      case op::kGetProperty: {
        const AsValue index = frame.Pop();
        const AsValue target = frame.Pop();
        const u32 which = static_cast<u32>(ToNumber(index));
        frame.Push(which < sizeof(kProperties) / sizeof(*kProperties)
                       ? GetMember(target, kProperties[which])
                       : AsValue::Undefined());
        break;
      }
      case op::kSetProperty: {
        const AsValue value = frame.Pop();
        const AsValue index = frame.Pop();
        const AsValue target = frame.Pop();
        const u32 which = static_cast<u32>(ToNumber(index));
        if (which < sizeof(kProperties) / sizeof(*kProperties))
          SetMember(target, kProperties[which], value);
        break;
      }
      case op::kDelete: {
        const AsValue name = frame.Pop();
        const AsValue target = frame.Pop();
        if (target.is_object() && Valid(target.object()))
          objects_[target.object()].props.erase(ToString(name));
        frame.Push(AsValue::Bool(true));
        break;
      }
      case op::kDelete2: {
        const AsValue name = frame.Pop();
        const base::String key = ToString(name);
        if (Valid(frame.locals))
          objects_[frame.locals].props.erase(key);
        frame.Push(AsValue::Bool(true));
        break;
      }

      case op::kInitObject: {
        const u32 pairs = static_cast<u32>(ToNumber(frame.Pop()));
        const u32 object = NewObject();
        for (u32 p = 0; p < pairs; ++p) {
          const AsValue value = frame.Pop();
          const AsValue name = frame.Pop();
          SetMember(AsValue::Obj(object), ToString(name), value);
        }
        frame.Push(AsValue::Obj(object));
        break;
      }
      case op::kInitArray: {
        const u32 n = static_cast<u32>(ToNumber(frame.Pop()));
        const u32 array = NewArray();
        // Element 0 is on top, the same way a call's arguments are pushed.
        for (u32 e = 0; e < n; ++e)
          SetMember(AsValue::Obj(array), base::Format("{}", e), frame.Pop());
        SetMember(AsValue::Obj(array), "length", AsValue::Number(n));
        frame.Push(AsValue::Obj(array));
        break;
      }

      case op::kCallFunction: {
        const AsValue name = frame.Pop();
        const u32 argc = static_cast<u32>(ToNumber(frame.Pop()));
        base::Vector<AsValue> args;
        for (u32 a = 0; a < argc; ++a)
          args.push_back(frame.Pop());
        const AsValue fn = ResolveVariable(frame, ToString(name));
        frame.Push(CallInternal(fn, frame.self, args, depth_ + 1));
        break;
      }
      case op::kCallMethod: {
        const AsValue name = frame.Pop();
        const AsValue target = frame.Pop();
        const u32 argc = static_cast<u32>(ToNumber(frame.Pop()));
        base::Vector<AsValue> args;
        for (u32 a = 0; a < argc; ++a)
          args.push_back(frame.Pop());
        // A method name that is empty, undefined or null means "call the
        // target itself" - which is how `super()` and a function held in a
        // variable are invoked.
        const bool call_target = name.is_undefined() ||
                                 name.type() == AsValue::Type::kNull ||
                                 ToString(name).empty();
        const AsValue fn = call_target ? target : GetMember(target, ToString(name));
        // `super()` runs the base constructor against the same object, so the
        // caller's `this` carries through rather than the super object.
        frame.Push(CallInternal(fn, call_target ? frame.self : target, args, depth_ + 1));
        break;
      }
      case op::kNewObject:
      case op::kNewMethod: {
        AsValue constructor;
        if (code == op::kNewMethod) {
          const AsValue name = frame.Pop();
          const AsValue target = frame.Pop();
          const bool call_target = name.is_undefined() ||
                                   name.type() == AsValue::Type::kNull ||
                                   ToString(name).empty();
          constructor = call_target ? target : GetMember(target, ToString(name));
        } else {
          constructor = ResolveVariable(frame, ToString(frame.Pop()));
        }
        const u32 argc = static_cast<u32>(ToNumber(frame.Pop()));
        base::Vector<AsValue> args;
        for (u32 a = 0; a < argc; ++a)
          args.push_back(frame.Pop());
        u32 prototype = 0;
        if (constructor.is_object()) {
          const AsValue proto = GetMember(constructor, "prototype");
          if (proto.is_object())
            prototype = proto.object();
        }
        const AsValue instance = AsValue::Obj(NewObject(prototype));
        SetMember(instance, "__constructor__", constructor);
        const AsValue returned = CallInternal(constructor, instance, args, depth_ + 1);
        frame.Push(returned.is_object() ? returned : instance);
        break;
      }

      case op::kReturn:
        frame.result = frame.Pop();
        frame.returned = true;
        return;

      case op::kDefineFunction:
      case op::kDefineFunction2: {
        const u32 body_first = IndexOfOffset(scripts_[script_index], i + 1,
                                             last - (i + 1), action.end);
        u32 body_count = 0;
        const u32 body_end = action.end + action.body_size;
        for (u32 b = body_first; b < last && b < scripts_[script_index].actions.size(); ++b) {
          if (scripts_[script_index].actions[b].offset >= body_end)
            break;
          ++body_count;
        }
        const u32 fn = NewObject(function_prototype_);
        objects_[fn].is_function = true;
        objects_[fn].scope = frame.locals;
        objects_[fn].body.script = script_index;
        objects_[fn].body.first = body_first;
        objects_[fn].body.count = body_count;
        objects_[fn].body.flags = action.function_flags;
        objects_[fn].body.register_count = action.register_count;
        objects_[fn].body.params = action.strings;
        objects_[fn].body.param_registers = action.param_registers;
        objects_[fn].body.pool = frame.pool;
        // Every function is a potential constructor, so it carries a prototype.
        SetMember(AsValue::Obj(fn), "prototype", AsValue::Obj(NewObject()));
        if (action.name.empty())
          frame.Push(AsValue::Obj(fn));
        else
          AssignVariable(frame, action.name, AsValue::Obj(fn));
        next = body_first + body_count;
        break;
      }

      case op::kWith: {
        const AsValue target = frame.Pop();
        const u32 body_first = IndexOfOffset(scripts_[script_index], i + 1,
                                             last - (i + 1), action.end);
        u32 body_count = 0;
        const u32 body_end = action.end + action.body_size;
        for (u32 b = body_first; b < last && b < scripts_[script_index].actions.size(); ++b) {
          if (scripts_[script_index].actions[b].offset >= body_end)
            break;
          ++body_count;
        }
        if (target.is_object())
          frame.with_chain.push_back(target.object());
        Execute(script_index, body_first, body_count, frame);
        if (target.is_object() && !frame.with_chain.empty())
          frame.with_chain.pop_back();
        next = body_first + body_count;
        break;
      }

      // Try/Catch/Finally. The three blocks sit inline after the action, so a
      // machine that just falls through runs all three: the catch block starts
      // by storing an exception nobody threw, and the stack is out of step for
      // the rest of the function. That is what silently stopped a CLIK button
      // from joining its group and a list row from getting its label.
      //
      // Nothing in these menus throws, so the catch block is skipped and the
      // finally block runs after the body, which is what happens when a try
      // completes.
      case op::kTry: {
        const AsScript& code = scripts_[script_index];
        const u32 body_first = IndexOfOffset(code, i + 1, last - (i + 1), action.end);
        const u32 catch_at = action.end + action.body_size;
        const u32 finally_at = catch_at + action.word_arg;
        const u32 end_at = finally_at + action.param_count;
        const u32 catch_first = IndexOfOffset(code, body_first, last - body_first, catch_at);
        const u32 finally_first =
            IndexOfOffset(code, body_first, last - body_first, finally_at);
        const u32 end_first = IndexOfOffset(code, body_first, last - body_first, end_at);
        if (catch_first > body_first)
          Execute(script_index, body_first, catch_first - body_first, frame);
        if (action.param_count != 0 && end_first > finally_first)
          Execute(script_index, finally_first, end_first - finally_first, frame);
        next = end_first;
        break;
      }

      case op::kJump:
        next = IndexOfOffset(scripts_[script_index], first, count,
                             static_cast<u32>(static_cast<i32>(action.end) + action.jump));
        break;
      case op::kIf: {
        const AsValue condition = frame.Pop();
        if (ToBool(condition))
          next = IndexOfOffset(scripts_[script_index], first, count,
                               static_cast<u32>(static_cast<i32>(action.end) + action.jump));
        break;
      }

      case op::kAdd:
        frame.Push(AsValue::Number(ToNumber(frame.Pop()) + ToNumber(frame.Pop())));
        break;
      case op::kAdd2: {
        const AsValue b = frame.Pop();
        const AsValue a = frame.Pop();
        if (a.is_string() || b.is_string())
          frame.Push(AsValue::Str(ToString(a) + ToString(b)));
        else
          frame.Push(AsValue::Number(ToNumber(a) + ToNumber(b)));
        break;
      }
      case op::kStringAdd: {
        const AsValue b = frame.Pop();
        const AsValue a = frame.Pop();
        frame.Push(AsValue::Str(ToString(a) + ToString(b)));
        break;
      }
      case op::kSubtract: {
        const f64 b = ToNumber(frame.Pop());
        frame.Push(AsValue::Number(ToNumber(frame.Pop()) - b));
        break;
      }
      case op::kMultiply:
        frame.Push(AsValue::Number(ToNumber(frame.Pop()) * ToNumber(frame.Pop())));
        break;
      case op::kDivide: {
        const f64 b = ToNumber(frame.Pop());
        frame.Push(AsValue::Number(ToNumber(frame.Pop()) / b));
        break;
      }
      case op::kModulo: {
        const f64 b = ToNumber(frame.Pop());
        frame.Push(AsValue::Number(std::fmod(ToNumber(frame.Pop()), b)));
        break;
      }
      case op::kIncrement:
        frame.Push(AsValue::Number(ToNumber(frame.Pop()) + 1));
        break;
      case op::kDecrement:
        frame.Push(AsValue::Number(ToNumber(frame.Pop()) - 1));
        break;

      case op::kEquals: {
        const f64 b = ToNumber(frame.Pop());
        frame.Push(AsValue::Bool(ToNumber(frame.Pop()) == b));
        break;
      }
      case op::kEquals2: {
        const AsValue b = frame.Pop();
        const AsValue a = frame.Pop();
        bool equal = false;
        const bool a_nullish = a.type() == AsValue::Type::kUndefined ||
                               a.type() == AsValue::Type::kNull;
        const bool b_nullish = b.type() == AsValue::Type::kUndefined ||
                               b.type() == AsValue::Type::kNull;
        if (a_nullish || b_nullish)
          equal = a_nullish && b_nullish;
        else if (a.is_object() && b.is_object())
          equal = a.object() == b.object();
        else if (a.is_string() && b.is_string())
          equal = a.string() == b.string();
        else
          equal = ToNumber(a) == ToNumber(b);
        frame.Push(AsValue::Bool(equal));
        break;
      }
      case op::kStrictEquals: {
        const AsValue b = frame.Pop();
        const AsValue a = frame.Pop();
        bool equal = a.type() == b.type();
        if (equal) {
          switch (a.type()) {
            case AsValue::Type::kObject:
              equal = a.object() == b.object();
              break;
            case AsValue::Type::kString:
              equal = a.string() == b.string();
              break;
            case AsValue::Type::kNumber:
              equal = a.raw_number() == b.raw_number();
              break;
            case AsValue::Type::kBool:
              equal = a.raw_bool() == b.raw_bool();
              break;
            default:
              break;
          }
        }
        frame.Push(AsValue::Bool(equal));
        break;
      }
      case op::kLess: {
        const f64 b = ToNumber(frame.Pop());
        frame.Push(AsValue::Bool(ToNumber(frame.Pop()) < b));
        break;
      }
      case op::kLess2: {
        const AsValue b = frame.Pop();
        const AsValue a = frame.Pop();
        if (a.is_string() && b.is_string())
          frame.Push(AsValue::Bool(a.string() < b.string()));
        else
          frame.Push(AsValue::Bool(ToNumber(a) < ToNumber(b)));
        break;
      }
      case op::kGreater: {
        const AsValue b = frame.Pop();
        const AsValue a = frame.Pop();
        if (a.is_string() && b.is_string())
          frame.Push(AsValue::Bool(b.string() < a.string()));
        else
          frame.Push(AsValue::Bool(ToNumber(a) > ToNumber(b)));
        break;
      }
      case op::kStringEquals: {
        const base::String b = ToString(frame.Pop());
        frame.Push(AsValue::Bool(ToString(frame.Pop()) == b));
        break;
      }
      case op::kStringLess: {
        const base::String b = ToString(frame.Pop());
        frame.Push(AsValue::Bool(ToString(frame.Pop()) < b));
        break;
      }
      case op::kStringGreater: {
        const base::String b = ToString(frame.Pop());
        frame.Push(AsValue::Bool(b < ToString(frame.Pop())));
        break;
      }

      case op::kNot:
        frame.Push(AsValue::Bool(!ToBool(frame.Pop())));
        break;
      case op::kAnd: {
        const bool b = ToBool(frame.Pop());
        frame.Push(AsValue::Bool(ToBool(frame.Pop()) && b));
        break;
      }
      case op::kOr: {
        const bool b = ToBool(frame.Pop());
        frame.Push(AsValue::Bool(ToBool(frame.Pop()) || b));
        break;
      }

      case op::kBitAnd: {
        const i32 b = static_cast<i32>(ToNumber(frame.Pop()));
        frame.Push(AsValue::Number(static_cast<i32>(ToNumber(frame.Pop())) & b));
        break;
      }
      case op::kBitOr: {
        const i32 b = static_cast<i32>(ToNumber(frame.Pop()));
        frame.Push(AsValue::Number(static_cast<i32>(ToNumber(frame.Pop())) | b));
        break;
      }
      case op::kBitXor: {
        const i32 b = static_cast<i32>(ToNumber(frame.Pop()));
        frame.Push(AsValue::Number(static_cast<i32>(ToNumber(frame.Pop())) ^ b));
        break;
      }
      case op::kBitLShift: {
        const i32 b = static_cast<i32>(ToNumber(frame.Pop())) & 31;
        frame.Push(AsValue::Number(static_cast<i32>(ToNumber(frame.Pop())) << b));
        break;
      }
      case op::kBitRShift: {
        const i32 b = static_cast<i32>(ToNumber(frame.Pop())) & 31;
        frame.Push(AsValue::Number(static_cast<i32>(ToNumber(frame.Pop())) >> b));
        break;
      }
      case op::kBitURShift: {
        const i32 b = static_cast<i32>(ToNumber(frame.Pop())) & 31;
        frame.Push(AsValue::Number(static_cast<u32>(ToNumber(frame.Pop())) >> b));
        break;
      }

      case op::kToNumber:
        frame.Push(AsValue::Number(ToNumber(frame.Pop())));
        break;
      case op::kToString:
        frame.Push(AsValue::Str(ToString(frame.Pop())));
        break;
      case op::kToInteger:
        frame.Push(AsValue::Number(static_cast<f64>(static_cast<i64>(ToNumber(frame.Pop())))));
        break;
      case op::kTypeOf: {
        const AsValue v = frame.Pop();
        const char* name = "undefined";
        switch (v.type()) {
          case AsValue::Type::kNull:
            name = "null";
            break;
          case AsValue::Type::kBool:
            name = "boolean";
            break;
          case AsValue::Type::kNumber:
            name = "number";
            break;
          case AsValue::Type::kString:
            name = "string";
            break;
          case AsValue::Type::kObject:
            name = Valid(v.object()) && objects_[v.object()].is_function ? "function"
                   : Valid(v.object()) && objects_[v.object()].is_movie_clip
                       ? "movieclip"
                       : "object";
            break;
          case AsValue::Type::kUndefined:
            break;
        }
        frame.Push(AsValue::Str(name));
        break;
      }

      case op::kStringLength:
      case op::kMbStringLength:
        frame.Push(AsValue::Number(static_cast<f64>(ToString(frame.Pop()).size())));
        break;
      case op::kStringExtract:
      case op::kMbStringExtract: {
        const i64 length = static_cast<i64>(ToNumber(frame.Pop()));
        const i64 start = static_cast<i64>(ToNumber(frame.Pop()));
        const base::String s = ToString(frame.Pop());
        base::String out;
        for (i64 k = 0; k < length; ++k) {
          const i64 at = start - 1 + k;  // 1-based in the language
          if (at >= 0 && at < static_cast<i64>(s.size()))
            out.push_back(s[static_cast<mem_size>(at)]);
        }
        frame.Push(AsValue::Str(out));
        break;
      }
      case op::kCharToAscii:
      case op::kMbCharToAscii: {
        const base::String s = ToString(frame.Pop());
        frame.Push(AsValue::Number(s.empty() ? 0 : static_cast<u8>(s[0])));
        break;
      }
      case op::kAsciiToChar:
      case op::kMbAsciiToChar: {
        base::String out;
        out.push_back(static_cast<char>(static_cast<i32>(ToNumber(frame.Pop()))));
        frame.Push(AsValue::Str(out));
        break;
      }

      case op::kTrace:
        traces_.push_back(ToString(frame.Pop()));
        break;

      case op::kExtends: {
        const AsValue super = frame.Pop();
        const AsValue subclass = frame.Pop();
        const AsValue super_proto = GetMember(super, "prototype");
        const u32 proto = NewObject(super_proto.is_object() ? super_proto.object() : 0);
        SetMember(AsValue::Obj(proto), "__constructor__", super);
        SetMember(subclass, "prototype", AsValue::Obj(proto));
        break;
      }
      case op::kInstanceOf: {
        const AsValue klass = frame.Pop();
        const AsValue instance = frame.Pop();
        bool is = false;
        if (instance.is_object() && klass.is_object()) {
          const AsValue proto = GetMember(klass, "prototype");
          u32 current = Valid(instance.object()) ? objects_[instance.object()].prototype : 0;
          for (u32 guard = 0; guard < 64 && Valid(current); ++guard) {
            if (proto.is_object() && current == proto.object()) {
              is = true;
              break;
            }
            current = objects_[current].prototype;
          }
        }
        frame.Push(AsValue::Bool(is));
        break;
      }
      case op::kCastOp: {
        const AsValue value = frame.Pop();
        frame.Pop();  // the class; a failed cast yields null, and nothing checks
        frame.Push(value);
        break;
      }
      case op::kImplementsOp: {
        const u32 n = static_cast<u32>(ToNumber(frame.Pop()));
        for (u32 k = 0; k < n; ++k)
          frame.Pop();
        frame.Pop();
        break;
      }

      case op::kEnumerate2: {
        const AsValue target = frame.Pop();
        frame.Push(AsValue::Null());  // the terminator the loop stops on
        if (target.is_object() && Valid(target.object())) {
          const base::Vector<base::String> keys = objects_[target.object()].order;
          for (mem_size k = keys.size(); k-- > 0;)
            frame.Push(AsValue::Str(keys[k]));
        }
        break;
      }

      case op::kRandomNumber:
        // Deterministic: a menu uses this for idle flourishes, and a run that
        // varies cannot be compared against another.
        frame.Push(AsValue::Number(0));
        break;
      case op::kGetTime:
        frame.Push(AsValue::Number(static_cast<f64>(steps_)));
        break;
      case op::kTargetPath:
        frame.Push(AsValue::Str(""));
        break;

      // The timeline opcodes are the statement forms of the clip methods, so
      // they go through the same place: whatever the host installed on the
      // movie-clip prototype. A frame is 1-based in the language.
      case op::kGotoFrame: {
        base::Vector<AsValue> args;
        args.push_back(AsValue::Number(static_cast<f64>(action.word_arg) + 1));
        CallInternal(GetMember(frame.self, "gotoAndStop"), frame.self, args, depth_ + 1);
        break;
      }
      case op::kGotoLabel: {
        base::Vector<AsValue> args;
        args.push_back(AsValue::Str(action.name));
        CallInternal(GetMember(frame.self, "gotoAndStop"), frame.self, args, depth_ + 1);
        break;
      }
      case op::kGotoFrame2: {
        const AsValue target = frame.Pop();
        base::Vector<AsValue> args;
        args.push_back(target);
        // Bit 0 of the flags says play rather than stop; both land on the same
        // native, which stops either way.
        CallInternal(GetMember(frame.self, "gotoAndStop"), frame.self, args, depth_ + 1);
        break;
      }
      case op::kCall:
        frame.Pop();
        break;
      case op::kNextFrame:
      case op::kPrevFrame: {
        const char* method = code == op::kNextFrame ? "nextFrame" : "prevFrame";
        CallInternal(GetMember(frame.self, method), frame.self,
                     base::Vector<AsValue>(), depth_ + 1);
        break;
      }
      case op::kStop:
      case op::kPlay:
      case op::kStopSounds:
      case op::kToggleQuality:
      case op::kWaitForFrame:
      case op::kWaitForFrame2:
      case op::kSetTarget:
      case op::kSetTarget2:
      case op::kEndDrag:
        break;
      case op::kStartDrag: {
        frame.Pop();
        const bool constrain = ToBool(frame.Pop());
        frame.Pop();
        if (constrain)
          for (int k = 0; k < 4; ++k)
            frame.Pop();
        break;
      }
      case op::kCloneSprite:
        frame.Pop();
        frame.Pop();
        frame.Pop();
        break;
      case op::kRemoveSprite:
        frame.Pop();
        break;
      case op::kGetUrl2:
        frame.Pop();
        frame.Pop();
        break;
      case op::kThrow:
        frame.Pop();
        break;

      default:
        // An opcode with no effect modelled here still has to leave the stack
        // as the compiler expects, and the ones that reach this point take no
        // operands off it.
        break;
    }
    i = next;
  }
}

// --- standard library -------------------------------------------------------

namespace {

AsValue NativeTrace(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  (void)vm;
  (void)args;
  return AsValue::Undefined();
}

// An array's length as an index bound. A script reaches these methods with
// `length` still unset often enough to matter (`ClearList` splices an array the
// component never initialised), and casting that NaN straight to i64 is
// undefined, so it has to be pinned here rather than at each use.
i64 ArrayLength(Vm& vm, const AsValue& self) {
  const f64 n = vm.ToNumber(vm.GetMember(self, "length"));
  if (std::isnan(n) || n <= 0)
    return 0;
  return static_cast<i64>(n);
}

// A count argument, with the same NaN pin.
i64 ArrayIndexArg(Vm& vm, const AsValue& value, i64 fallback) {
  const f64 n = vm.ToNumber(value);
  return std::isnan(n) ? fallback : static_cast<i64>(n);
}

// new Array(): empty, new Array(n): n slots, new Array(a, b, ...): those
// elements. The menus build their fixed tables this way (a journal's pages are
// `new Array(QuestsFader.Page_mc, ...)`), so a constructor that ignored its
// arguments left every one of those tables empty.
AsValue ArrayCtor(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  if (self.is_object() && vm.Valid(self.object()))
    vm.Get(self.object()).is_array = true;
  if (args.size() == 1 && args[0].type() == AsValue::Type::kNumber) {
    vm.SetMember(self, "length", args[0]);
    return AsValue::Undefined();
  }
  for (mem_size i = 0; i < args.size(); ++i)
    vm.SetMember(self, base::Format("{}", i), args[i]);
  vm.SetMember(self, "length", AsValue::Number(static_cast<f64>(args.size())));
  return AsValue::Undefined();
}

AsValue ArrayPush(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const AsValue length = vm.GetMember(self, "length");
  f64 n = vm.ToNumber(length);
  if (std::isnan(n))
    n = 0;
  for (const AsValue& arg : args)
    vm.SetMember(self, base::Format("{}", static_cast<i64>(n++)), arg);
  vm.SetMember(self, "length", AsValue::Number(n));
  return AsValue::Number(n);
}

AsValue ArrayPop(Vm& vm, const AsValue& self, const base::Vector<AsValue>&) {
  const f64 n = vm.ToNumber(vm.GetMember(self, "length"));
  if (n <= 0)
    return AsValue::Undefined();
  const base::String key = base::Format("{}", static_cast<i64>(n) - 1);
  const AsValue out = vm.GetMember(self, key);
  if (self.is_object() && vm.Valid(self.object()))
    vm.Get(self.object()).props.erase(key);
  vm.SetMember(self, "length", AsValue::Number(n - 1));
  return out;
}

AsValue ArrayJoin(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const base::String sep = args.empty() ? base::String(",") : vm.ToString(args[0]);
  const i64 n = ArrayLength(vm, self);
  base::String out;
  for (i64 i = 0; i < n; ++i) {
    if (i)
      out += sep;
    out += vm.ToString(vm.GetMember(self, base::Format("{}", i)));
  }
  return AsValue::Str(out);
}

AsValue MathFloor(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  return AsValue::Number(std::floor(args.empty() ? 0 : vm.ToNumber(args[0])));
}
AsValue MathCeil(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  return AsValue::Number(std::ceil(args.empty() ? 0 : vm.ToNumber(args[0])));
}
AsValue MathRound(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  return AsValue::Number(std::floor((args.empty() ? 0 : vm.ToNumber(args[0])) + 0.5));
}
AsValue MathAbs(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  return AsValue::Number(std::fabs(args.empty() ? 0 : vm.ToNumber(args[0])));
}
AsValue MathMin(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  if (args.empty())
    return AsValue::Number(std::nan(""));
  f64 out = vm.ToNumber(args[0]);
  for (mem_size i = 1; i < args.size(); ++i)
    out = base::Min(out, vm.ToNumber(args[i]));
  return AsValue::Number(out);
}
AsValue MathMax(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  if (args.empty())
    return AsValue::Number(std::nan(""));
  f64 out = vm.ToNumber(args[0]);
  for (mem_size i = 1; i < args.size(); ++i)
    out = base::Max(out, vm.ToNumber(args[i]));
  return AsValue::Number(out);
}
AsValue MathSqrt(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  return AsValue::Number(std::sqrt(args.empty() ? 0 : vm.ToNumber(args[0])));
}
AsValue MathPow(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  const f64 a = args.size() > 0 ? vm.ToNumber(args[0]) : 0;
  const f64 b = args.size() > 1 ? vm.ToNumber(args[1]) : 0;
  return AsValue::Number(std::pow(a, b));
}
AsValue MathRandom(Vm&, const AsValue&, const base::Vector<AsValue>&) {
  return AsValue::Number(0);  // deterministic, so two runs can be compared
}

AsValue StringSubstr(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const base::String s = vm.ToString(self);
  i64 start = args.size() > 0 ? static_cast<i64>(vm.ToNumber(args[0])) : 0;
  if (start < 0)
    start = base::Max<i64>(0, static_cast<i64>(s.size()) + start);
  i64 length = args.size() > 1 ? static_cast<i64>(vm.ToNumber(args[1]))
                               : static_cast<i64>(s.size()) - start;
  base::String out;
  for (i64 i = 0; i < length; ++i) {
    const i64 at = start + i;
    if (at >= 0 && at < static_cast<i64>(s.size()))
      out.push_back(s[static_cast<mem_size>(at)]);
  }
  return AsValue::Str(out);
}

AsValue StringIndexOf(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const base::String s = vm.ToString(self);
  const base::String needle = args.empty() ? base::String() : vm.ToString(args[0]);
  const mem_size at = s.find(needle);
  return AsValue::Number(at == base::String::npos ? -1 : static_cast<f64>(at));
}

AsValue StringToUpper(Vm& vm, const AsValue& self, const base::Vector<AsValue>&) {
  base::String s = vm.ToString(self);
  for (mem_size i = 0; i < s.size(); ++i)
    if (s[i] >= 'a' && s[i] <= 'z')
      s[i] = static_cast<char>(s[i] - 'a' + 'A');
  return AsValue::Str(s);
}

AsValue GlobalParseInt(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  if (args.empty())
    return AsValue::Number(std::nan(""));
  return AsValue::Number(static_cast<f64>(static_cast<i64>(vm.ToNumber(args[0]))));
}

AsValue GlobalIsNaN(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  return AsValue::Bool(args.empty() || std::isnan(vm.ToNumber(args[0])));
}

AsValue NoOp(Vm&, const AsValue&, const base::Vector<AsValue>&) {
  return AsValue::Undefined();
}

AsValue FunctionApply(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const AsValue this_arg = args.size() > 0 ? args[0] : AsValue::Undefined();
  base::Vector<AsValue> call_args;
  if (args.size() > 1 && args[1].is_object()) {
    const i64 n = ArrayLength(vm, args[1]);
    for (i64 i = 0; i < n; ++i)
      call_args.push_back(vm.GetMember(args[1], base::Format("{}", i)));
  }
  return vm.Call(self, this_arg, call_args);
}

AsValue FunctionCall(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const AsValue this_arg = args.size() > 0 ? args[0] : AsValue::Undefined();
  base::Vector<AsValue> call_args;
  for (mem_size i = 1; i < args.size(); ++i)
    call_args.push_back(args[i]);
  return vm.Call(self, this_arg, call_args);
}

AsValue ArrayUnshift(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const i64 n = ArrayLength(vm, self);
  const i64 shift = static_cast<i64>(args.size());
  for (i64 i = n - 1; i >= 0; --i)
    vm.SetMember(self, base::Format("{}", i + shift),
                 vm.GetMember(self, base::Format("{}", i)));
  for (mem_size i = 0; i < args.size(); ++i)
    vm.SetMember(self, base::Format("{}", i), args[i]);
  vm.SetMember(self, "length", AsValue::Number(static_cast<f64>(n + shift)));
  return AsValue::Number(static_cast<f64>(n + shift));
}

// A start/end pair the way the language resolves them: negative counts from the
// end, out of range clamps, an absent end means "to the end".
base::Pair<i64, i64> SliceRange(Vm& vm, const base::Vector<AsValue>& args, i64 n) {
  auto clamp = [n](i64 i) {
    if (i < 0)
      i += n;
    return i < 0 ? 0 : (i > n ? n : i);
  };
  const i64 first = args.empty() ? 0 : clamp(ArrayIndexArg(vm, args[0], 0));
  const i64 last =
      args.size() > 1 && !args[1].is_undefined() ? clamp(ArrayIndexArg(vm, args[1], n)) : n;
  return base::Pair<i64, i64>{first, last > first ? last : first};
}

// Copies a run of elements into a fresh array. GameDelegate dispatches through
// this: `receiveCall` forwards `arguments.slice(1)` to the movie's own handler.
AsValue ArraySlice(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const i64 n = ArrayLength(vm, self);
  const base::Pair<i64, i64> range = SliceRange(vm, args, n);
  const AsValue out = AsValue::Obj(vm.NewArray());
  i64 written = 0;
  for (i64 i = range.first; i < range.second; ++i)
    vm.SetMember(out, base::Format("{}", written++), vm.GetMember(self, base::Format("{}", i)));
  vm.SetMember(out, "length", AsValue::Number(static_cast<f64>(written)));
  return out;
}

// splice(start, count, ...inserted): removes a run and puts one in its place,
// returning what came out. The lists use it to keep their entry arrays in sync
// with what the game sent.
AsValue ArraySplice(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const i64 n = ArrayLength(vm, self);
  i64 start = args.empty() ? 0 : ArrayIndexArg(vm, args[0], 0);
  if (start < 0)
    start += n;
  start = start < 0 ? 0 : (start > n ? n : start);
  i64 remove = args.size() > 1 ? ArrayIndexArg(vm, args[1], n - start) : n - start;
  remove = remove < 0 ? 0 : (remove > n - start ? n - start : remove);

  const AsValue out = AsValue::Obj(vm.NewArray());
  for (i64 i = 0; i < remove; ++i)
    vm.SetMember(out, base::Format("{}", i), vm.GetMember(self, base::Format("{}", start + i)));
  vm.SetMember(out, "length", AsValue::Number(static_cast<f64>(remove)));

  base::Vector<AsValue> tail;
  for (mem_size i = 2; i < args.size(); ++i)
    tail.push_back(args[i]);
  for (i64 i = start + remove; i < n; ++i)
    tail.push_back(vm.GetMember(self, base::Format("{}", i)));
  for (mem_size i = 0; i < tail.size(); ++i)
    vm.SetMember(self, base::Format("{}", start + static_cast<i64>(i)), tail[i]);

  const i64 length = start + static_cast<i64>(tail.size());
  if (self.is_object() && vm.Valid(self.object()))
    for (i64 i = length; i < n; ++i)
      vm.Get(self.object()).props.erase(base::Format("{}", i));
  vm.SetMember(self, "length", AsValue::Number(static_cast<f64>(length)));
  return out;
}

AsValue ArrayShift(Vm& vm, const AsValue& self, const base::Vector<AsValue>&) {
  base::Vector<AsValue> args;
  args.push_back(AsValue::Number(0));
  args.push_back(AsValue::Number(1));
  const AsValue removed = ArraySplice(vm, self, args);
  return vm.GetMember(removed, "0");
}

AsValue ArrayConcat(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const AsValue out = AsValue::Obj(vm.NewArray());
  i64 written = 0;
  auto append = [&](const AsValue& value) {
    const bool spread = value.is_object() && vm.Valid(value.object()) &&
                        vm.Get(value.object()).is_array;
    if (!spread) {
      vm.SetMember(out, base::Format("{}", written++), value);
      return;
    }
    const i64 n = ArrayLength(vm, value);
    for (i64 i = 0; i < n; ++i)
      vm.SetMember(out, base::Format("{}", written++),
                   vm.GetMember(value, base::Format("{}", i)));
  };
  append(self);
  for (const AsValue& arg : args)
    append(arg);
  vm.SetMember(out, "length", AsValue::Number(static_cast<f64>(written)));
  return out;
}

AsValue ArrayIndexOf(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  if (args.empty())
    return AsValue::Number(-1);
  const i64 n = ArrayLength(vm, self);
  for (i64 i = 0; i < n; ++i) {
    const AsValue element = vm.GetMember(self, base::Format("{}", i));
    if (element.type() != args[0].type())
      continue;
    switch (element.type()) {
      case AsValue::Type::kObject:
        if (element.object() == args[0].object())
          return AsValue::Number(static_cast<f64>(i));
        break;
      case AsValue::Type::kString:
        if (element.string() == args[0].string())
          return AsValue::Number(static_cast<f64>(i));
        break;
      case AsValue::Type::kNumber:
        if (element.raw_number() == args[0].raw_number())
          return AsValue::Number(static_cast<f64>(i));
        break;
      case AsValue::Type::kBool:
        if (element.raw_bool() == args[0].raw_bool())
          return AsValue::Number(static_cast<f64>(i));
        break;
      default:
        return AsValue::Number(static_cast<f64>(i));
    }
  }
  return AsValue::Number(-1);
}

// sort(comparator?). Insertion sort: the arrays here are menu-sized, and it
// keeps the comparator calls in the order the script would see them.
AsValue ArraySort(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const i64 n = ArrayLength(vm, self);
  const AsValue comparator = args.empty() ? AsValue::Undefined() : args[0];
  const bool custom = comparator.is_object() && vm.Valid(comparator.object()) &&
                      vm.Get(comparator.object()).is_function;
  for (i64 i = 1; i < n; ++i) {
    const AsValue value = vm.GetMember(self, base::Format("{}", i));
    i64 j = i - 1;
    for (; j >= 0; --j) {
      const AsValue other = vm.GetMember(self, base::Format("{}", j));
      f64 order = 0;
      if (custom) {
        base::Vector<AsValue> pair;
        pair.push_back(other);
        pair.push_back(value);
        order = vm.ToNumber(vm.Call(comparator, AsValue::Undefined(), pair));
      } else {
        order = vm.ToString(other) > vm.ToString(value) ? 1 : -1;
      }
      if (order <= 0)
        break;
      vm.SetMember(self, base::Format("{}", j + 1), other);
    }
    vm.SetMember(self, base::Format("{}", j + 1), value);
  }
  return self;
}

// flash.external.ExternalInterface.call(name, ...): the one door a Scaleform
// menu has to its host. gfx.io.GameDelegate is written on top of it.
AsValue ExternalCall(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  if (args.empty())
    return AsValue::Undefined();
  base::Vector<AsValue> rest;
  for (mem_size i = 1; i < args.size(); ++i)
    rest.push_back(args[i]);
  return vm.DispatchExternal(vm.ToString(args[0]), rest);
}

// Object.prototype.addProperty(name, getter, setter): the shipped scripts use
// this for nearly every public field of a component.
// A text field's measurements. Without a text engine these are estimates from
// the character count; the scripts feed them into layout (PositionButtons walks
// getLineMetrics().width), so returning nothing would make those positions NaN.
// A host that has laid the text out should overwrite textWidth on the field.
AsValue TextFieldLineMetrics(Vm& vm, const AsValue& self,
                             const base::Vector<AsValue>&) {
  const base::String text = vm.ToString(vm.GetMember(self, "text"));
  const u32 metrics = vm.NewObject();
  const f64 width = static_cast<f64>(text.size()) * 8.0;
  vm.SetMember(AsValue::Obj(metrics), "width", AsValue::Number(width));
  vm.SetMember(AsValue::Obj(metrics), "height", AsValue::Number(16));
  vm.SetMember(AsValue::Obj(metrics), "ascent", AsValue::Number(12));
  vm.SetMember(AsValue::Obj(metrics), "descent", AsValue::Number(4));
  vm.SetMember(AsValue::Obj(metrics), "leading", AsValue::Number(0));
  return AsValue::Obj(metrics);
}

AsValue TextFieldGetFormat(Vm& vm, const AsValue& self, const base::Vector<AsValue>&) {
  const AsValue stored = vm.GetMember(self, "__format");
  if (stored.is_object())
    return stored;
  const u32 format = vm.NewObject();
  vm.SetMember(AsValue::Obj(format), "letterSpacing", AsValue::Number(0));
  vm.SetMember(AsValue::Obj(format), "kerning", AsValue::Bool(false));
  vm.SetMember(AsValue::Obj(format), "align", AsValue::Str("left"));
  return AsValue::Obj(format);
}

AsValue TextFieldSetFormat(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  if (!args.empty())
    vm.SetMember(self, "__format", args[args.size() - 1]);
  return AsValue::Undefined();
}

AsValue ObjectAddProperty(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  if (args.size() < 2 || !self.is_object() || !vm.Valid(self.object()))
    return AsValue::Bool(false);
  AsAccessor accessor;
  accessor.getter = args[1];
  accessor.setter = args.size() > 2 ? args[2] : AsValue::Undefined();
  vm.Get(self.object()).accessors[vm.ToString(args[0])] = accessor;
  return AsValue::Bool(true);
}

AsValue ObjectRegisterClass(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  if (args.size() < 2)
    return AsValue::Bool(false);
  vm.RegisterClass(vm.ToString(args[0]), args[1]);
  return AsValue::Bool(true);
}

// setInterval(fn, ms, ...) and setInterval(scope, "method", ms, ...), which are
// both spellings the shipped scripts use.
AsValue SetIntervalNative(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  if (args.size() < 2)
    return AsValue::Number(0);
  AsValue fn = args[0];
  AsValue self = AsValue::Undefined();
  mem_size next = 1;
  if (args[1].is_string()) {
    self = args[0];
    fn = vm.GetMember(args[0], args[1].string());
    next = 2;
  }
  if (next >= args.size())
    return AsValue::Number(0);
  const f64 interval = vm.ToNumber(args[next]);
  base::Vector<AsValue> extra;
  for (mem_size i = next + 1; i < args.size(); ++i)
    extra.push_back(args[i]);
  return AsValue::Number(
      static_cast<f64>(vm.AddTimer(fn, self, base::move(extra), interval, false)));
}

AsValue SetTimeoutNative(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  const AsValue id = SetIntervalNative(vm, self, args);
  // Same shape, fires once. The id came back from AddTimer, so flip it there.
  vm.MakeTimerOneShot(static_cast<u32>(vm.ToNumber(id)));
  return id;
}

AsValue ClearIntervalNative(Vm& vm, const AsValue&, const base::Vector<AsValue>& args) {
  if (!args.empty())
    vm.ClearTimer(static_cast<u32>(vm.ToNumber(args[0])));
  return AsValue::Undefined();
}

}  // namespace

void Vm::InstallStandardLibrary() {
  const AsValue g = AsValue::Obj(global_);

  SetMember(g, "trace", AsValue::Obj(NewNative(NativeTrace)));
  SetMember(g, "parseInt", AsValue::Obj(NewNative(GlobalParseInt)));
  SetMember(g, "parseFloat", AsValue::Obj(NewNative(GlobalParseInt)));
  SetMember(g, "isNaN", AsValue::Obj(NewNative(GlobalIsNaN)));
  SetMember(g, "ASSetPropFlags", AsValue::Obj(NewNative(NoOp)));
  SetMember(g, "setInterval", AsValue::Obj(NewNative(SetIntervalNative)));
  SetMember(g, "setTimeout", AsValue::Obj(NewNative(SetTimeoutNative)));
  SetMember(g, "clearInterval", AsValue::Obj(NewNative(ClearIntervalNative)));
  SetMember(g, "clearTimeout", AsValue::Obj(NewNative(ClearIntervalNative)));
  SetMember(g, "updateAfterEvent", AsValue::Obj(NewNative(NoOp)));
  SetMember(g, "undefined", AsValue::Undefined());
  SetMember(g, "NaN", AsValue::Number(std::nan("")));
  SetMember(g, "Infinity", AsValue::Number(HUGE_VAL));

  // Object: its prototype is the root of every chain.
  const u32 object_ctor = NewNative(NoOp);
  const u32 object_proto = NewObject();
  SetMember(AsValue::Obj(object_ctor), "prototype", AsValue::Obj(object_proto));
  SetMember(AsValue::Obj(object_proto), "addProperty",
            AsValue::Obj(NewNative(ObjectAddProperty)));
  SetMember(AsValue::Obj(object_ctor), "registerClass",
            AsValue::Obj(NewNative(ObjectRegisterClass)));
  SetMember(g, "Object", AsValue::Obj(object_ctor));

  // Array, with the handful of methods the menus actually call.
  const u32 array_proto = NewObject(object_proto);
  SetMember(AsValue::Obj(array_proto), "push", AsValue::Obj(NewNative(ArrayPush)));
  SetMember(AsValue::Obj(array_proto), "pop", AsValue::Obj(NewNative(ArrayPop)));
  SetMember(AsValue::Obj(array_proto), "join", AsValue::Obj(NewNative(ArrayJoin)));
  const u32 array_ctor = NewNative(ArrayCtor);
  SetMember(AsValue::Obj(array_ctor), "prototype", AsValue::Obj(array_proto));
  SetMember(g, "Array", AsValue::Obj(array_ctor));
  array_prototype_ = array_proto;

  const u32 string_proto = NewObject(object_proto);
  SetMember(AsValue::Obj(string_proto), "substr", AsValue::Obj(NewNative(StringSubstr)));
  SetMember(AsValue::Obj(string_proto), "substring", AsValue::Obj(NewNative(StringSubstr)));
  SetMember(AsValue::Obj(string_proto), "indexOf", AsValue::Obj(NewNative(StringIndexOf)));
  SetMember(AsValue::Obj(string_proto), "toUpperCase",
            AsValue::Obj(NewNative(StringToUpper)));
  const u32 string_ctor = NewNative(NoOp);
  SetMember(AsValue::Obj(string_ctor), "prototype", AsValue::Obj(string_proto));
  SetMember(g, "String", AsValue::Obj(string_ctor));

  const u32 math = NewObject(object_proto);
  SetMember(AsValue::Obj(math), "floor", AsValue::Obj(NewNative(MathFloor)));
  SetMember(AsValue::Obj(math), "ceil", AsValue::Obj(NewNative(MathCeil)));
  SetMember(AsValue::Obj(math), "round", AsValue::Obj(NewNative(MathRound)));
  SetMember(AsValue::Obj(math), "abs", AsValue::Obj(NewNative(MathAbs)));
  SetMember(AsValue::Obj(math), "min", AsValue::Obj(NewNative(MathMin)));
  SetMember(AsValue::Obj(math), "max", AsValue::Obj(NewNative(MathMax)));
  SetMember(AsValue::Obj(math), "sqrt", AsValue::Obj(NewNative(MathSqrt)));
  SetMember(AsValue::Obj(math), "pow", AsValue::Obj(NewNative(MathPow)));
  SetMember(AsValue::Obj(math), "random", AsValue::Obj(NewNative(MathRandom)));
  SetMember(AsValue::Obj(math), "PI", AsValue::Number(3.14159265358979323846));
  SetMember(g, "Math", AsValue::Obj(math));

  const u32 number_ctor = NewNative(NoOp);
  SetMember(AsValue::Obj(number_ctor), "prototype", AsValue::Obj(NewObject(object_proto)));
  SetMember(AsValue::Obj(number_ctor), "MAX_VALUE", AsValue::Number(1.7976931348623157e308));
  SetMember(AsValue::Obj(number_ctor), "MIN_VALUE", AsValue::Number(5e-324));
  SetMember(g, "Number", AsValue::Obj(number_ctor));

  const u32 boolean_ctor = NewNative(NoOp);
  SetMember(AsValue::Obj(boolean_ctor), "prototype", AsValue::Obj(NewObject(object_proto)));
  SetMember(g, "Boolean", AsValue::Obj(boolean_ctor));

  // Functions share a prototype, so `apply` and `call` resolve on any of them.
  // GameDelegate reaches its host through `ExternalInterface.call.apply(...)`.
  function_prototype_ = NewObject(object_proto);
  SetMember(AsValue::Obj(function_prototype_), "apply",
            AsValue::Obj(NewNative(FunctionApply)));
  SetMember(AsValue::Obj(function_prototype_), "call",
            AsValue::Obj(NewNative(FunctionCall)));
  const u32 function_ctor = NewNative(NoOp);
  SetMember(AsValue::Obj(function_ctor), "prototype", AsValue::Obj(function_prototype_));
  SetMember(g, "Function", AsValue::Obj(function_ctor));

  SetMember(AsValue::Obj(array_proto), "unshift", AsValue::Obj(NewNative(ArrayUnshift)));
  SetMember(AsValue::Obj(array_proto), "slice", AsValue::Obj(NewNative(ArraySlice)));
  SetMember(AsValue::Obj(array_proto), "splice", AsValue::Obj(NewNative(ArraySplice)));
  SetMember(AsValue::Obj(array_proto), "shift", AsValue::Obj(NewNative(ArrayShift)));
  SetMember(AsValue::Obj(array_proto), "concat", AsValue::Obj(NewNative(ArrayConcat)));
  SetMember(AsValue::Obj(array_proto), "indexOf", AsValue::Obj(NewNative(ArrayIndexOf)));
  SetMember(AsValue::Obj(array_proto), "sort", AsValue::Obj(NewNative(ArraySort)));

  // flash.external.ExternalInterface, the host bridge.
  const u32 external = NewObject(object_proto);
  SetMember(AsValue::Obj(external), "call", AsValue::Obj(NewNative(ExternalCall)));
  SetMember(AsValue::Obj(external), "addCallback", AsValue::Obj(NewNative(NoOp)));
  SetMember(AsValue::Obj(external), "available", AsValue::Bool(true));
  const u32 external_ns = NewObject(object_proto);
  SetMember(AsValue::Obj(external_ns), "ExternalInterface", AsValue::Obj(external));
  const u32 flash_ns = NewObject(object_proto);
  SetMember(AsValue::Obj(flash_ns), "external", AsValue::Obj(external_ns));
  SetMember(g, "flash", AsValue::Obj(flash_ns));

  // Text fields. `text` and `htmlText` are the same store, which is what the
  // scripts assume when they write one and read the other.
  text_field_prototype_ = NewObject(object_proto);
  SetMember(AsValue::Obj(text_field_prototype_), "getLineMetrics",
            AsValue::Obj(NewNative(TextFieldLineMetrics)));
  SetMember(AsValue::Obj(text_field_prototype_), "getTextFormat",
            AsValue::Obj(NewNative(TextFieldGetFormat)));
  SetMember(AsValue::Obj(text_field_prototype_), "setTextFormat",
            AsValue::Obj(NewNative(TextFieldSetFormat)));
  SetMember(AsValue::Obj(text_field_prototype_), "setNewTextFormat",
            AsValue::Obj(NewNative(TextFieldSetFormat)));
  SetMember(AsValue::Obj(text_field_prototype_), "removeTextField",
            AsValue::Obj(NewNative(NoOp)));
  SetMember(AsValue::Obj(text_field_prototype_), "replaceSel",
            AsValue::Obj(NewNative(NoOp)));
  const u32 text_field_ctor = NewNative(NoOp);
  SetMember(AsValue::Obj(text_field_ctor), "prototype",
            AsValue::Obj(text_field_prototype_));
  SetMember(g, "TextField", AsValue::Obj(text_field_ctor));

  const u32 text_format_ctor = NewNative(NoOp);
  SetMember(AsValue::Obj(text_format_ctor), "prototype",
            AsValue::Obj(NewObject(object_proto)));
  SetMember(g, "TextFormat", AsValue::Obj(text_format_ctor));

  // The player globals a menu reads while laying itself out. Stage.safeRect is
  // what the Lock helper insets a menu's chrome by.
  const u32 visible_rect = NewObject(object_proto);
  SetMember(AsValue::Obj(visible_rect), "x", AsValue::Number(0));
  SetMember(AsValue::Obj(visible_rect), "y", AsValue::Number(0));
  SetMember(AsValue::Obj(visible_rect), "width", AsValue::Number(1280));
  SetMember(AsValue::Obj(visible_rect), "height", AsValue::Number(720));
  const u32 safe_rect = NewObject(object_proto);
  SetMember(AsValue::Obj(safe_rect), "x", AsValue::Number(0));
  SetMember(AsValue::Obj(safe_rect), "y", AsValue::Number(0));
  const u32 stage_object = NewObject(object_proto);
  SetMember(AsValue::Obj(stage_object), "visibleRect", AsValue::Obj(visible_rect));
  SetMember(AsValue::Obj(stage_object), "safeRect", AsValue::Obj(safe_rect));
  SetMember(AsValue::Obj(stage_object), "width", AsValue::Number(1280));
  SetMember(AsValue::Obj(stage_object), "height", AsValue::Number(720));
  SetMember(AsValue::Obj(stage_object), "align", AsValue::Str("TL"));
  SetMember(AsValue::Obj(stage_object), "scaleMode", AsValue::Str("noScale"));
  SetMember(AsValue::Obj(stage_object), "addListener", AsValue::Obj(NewNative(NoOp)));
  SetMember(g, "Stage", AsValue::Obj(stage_object));

  for (const char* name : {"Mouse", "Key", "Selection"}) {
    const u32 listener = NewObject(object_proto);
    SetMember(AsValue::Obj(listener), "addListener", AsValue::Obj(NewNative(NoOp)));
    SetMember(AsValue::Obj(listener), "removeListener", AsValue::Obj(NewNative(NoOp)));
    SetMember(AsValue::Obj(listener), "setFocus", AsValue::Obj(NewNative(NoOp)));
    SetMember(AsValue::Obj(listener), "isDown", AsValue::Obj(NewNative(NoOp)));
    SetMember(g, name, AsValue::Obj(listener));
  }

  // Every clip inherits from this; the host hangs the clip API on it.
  movie_clip_prototype_ = NewObject(object_proto);
  const u32 movie_clip_ctor = NewNative(NoOp);
  SetMember(AsValue::Obj(movie_clip_ctor), "prototype", AsValue::Obj(movie_clip_prototype_));
  SetMember(g, "MovieClip", AsValue::Obj(movie_clip_ctor));

  object_prototype_ = object_proto;
  string_prototype_ = string_proto;
}

u32 Vm::AddTimer(const AsValue& fn, const AsValue& self, base::Vector<AsValue> args,
                 f64 interval_ms, bool once) {
  Timer timer;
  timer.fn = fn;
  timer.self = self;
  timer.args = base::move(args);
  timer.interval_ms = interval_ms < 1 ? 1 : interval_ms;
  timer.due_ms = now_ms_ + timer.interval_ms;
  timer.id = next_timer_id_++;
  timer.once = once;
  const u32 id = timer.id;
  timers_.push_back(base::move(timer));
  return id;
}

void Vm::MakeTimerOneShot(u32 id) {
  for (Timer& timer : timers_)
    if (timer.id == id)
      timer.once = true;
}

void Vm::ClearTimer(u32 id) {
  for (mem_size i = 0; i < timers_.size(); ++i) {
    if (timers_[i].id == id) {
      timers_.erase(i);
      return;
    }
  }
}

u32 Vm::Tick(f64 elapsed_ms) {
  now_ms_ += elapsed_ms;
  u32 fired = 0;
  // Copied before firing: a handler can add or clear timers, which would move
  // the vector underneath the walk.
  base::Vector<Timer> due;
  for (const Timer& timer : timers_)
    if (timer.due_ms <= now_ms_)
      due.push_back(timer);
  for (const Timer& timer : due) {
    bool still_live = false;
    for (Timer& live : timers_) {
      if (live.id != timer.id)
        continue;
      still_live = true;
      live.due_ms = now_ms_ + live.interval_ms;
    }
    if (!still_live)
      continue;
    ++fired;
    Call(timer.fn, timer.self, timer.args);
    if (timer.once)
      ClearTimer(timer.id);
  }
  return fired;
}

AsValue Vm::DispatchExternal(base::StringRef name, const base::Vector<AsValue>& args) {
  external_calls_.push_back(base::String(name));
  if (!external_handler_)
    return AsValue::Undefined();
  return external_handler_(external_user_, *this, name, args);
}

void Vm::RegisterClass(base::StringRef symbol, const AsValue& klass) {
  registered_classes_[base::String(symbol)] = klass;
}

AsValue Vm::RegisteredClass(base::StringRef symbol) const {
  const AsValue* found = registered_classes_.find(base::String(symbol));
  return found ? *found : AsValue::Undefined();
}

}  // namespace rx::swf
