#include "components/swf/avm2.h"

#include <base/memory/move.h>
#include <base/strings/format.h>

#include <cmath>

namespace rx::swf {
namespace {

// A menu's setup is short. This is a runaway guard, not a budget: the shipped
// screens settle in a few hundred thousand instructions.
constexpr u64 kMaxSteps = 8'000'000;
constexpr u32 kMaxDepth = 64;
// Stand-ins are made on demand, so a loop that walks a display tree can ask for
// them forever. A screen's tree is thousands of objects, not millions.
constexpr u32 kMaxObjects = 400'000;

// AVM2 opcodes, by the names the disassembler gives them. Switching on the byte
// keeps the machine readable against the format documentation.
namespace op {
constexpr u8 kNop = 0x02;
constexpr u8 kThrow = 0x03;
constexpr u8 kGetSuper = 0x04;
constexpr u8 kSetSuper = 0x05;
constexpr u8 kKill = 0x08;
constexpr u8 kLabel = 0x09;
constexpr u8 kIfNlt = 0x0c;
constexpr u8 kIfNle = 0x0d;
constexpr u8 kIfNgt = 0x0e;
constexpr u8 kIfNge = 0x0f;
constexpr u8 kJump = 0x10;
constexpr u8 kIfTrue = 0x11;
constexpr u8 kIfFalse = 0x12;
constexpr u8 kIfEq = 0x13;
constexpr u8 kIfNe = 0x14;
constexpr u8 kIfLt = 0x15;
constexpr u8 kIfLe = 0x16;
constexpr u8 kIfGt = 0x17;
constexpr u8 kIfGe = 0x18;
constexpr u8 kIfStrictEq = 0x19;
constexpr u8 kIfStrictNe = 0x1a;
constexpr u8 kLookupSwitch = 0x1b;
constexpr u8 kPushWith = 0x1c;
constexpr u8 kPopScope = 0x1d;
constexpr u8 kHasNext2 = 0x32;
constexpr u8 kNextName = 0x1e;
constexpr u8 kNextValue = 0x23;
constexpr u8 kPushByte = 0x24;
constexpr u8 kPushShort = 0x25;
constexpr u8 kPushTrue = 0x26;
constexpr u8 kPushFalse = 0x27;
constexpr u8 kPushNan = 0x28;
constexpr u8 kPop = 0x29;
constexpr u8 kDup = 0x2a;
constexpr u8 kSwap = 0x2b;
constexpr u8 kPushString = 0x2c;
constexpr u8 kPushInt = 0x2d;
constexpr u8 kPushUint = 0x2e;
constexpr u8 kPushDouble = 0x2f;
constexpr u8 kPushScope = 0x30;
constexpr u8 kNewFunction = 0x40;
constexpr u8 kCall = 0x41;
constexpr u8 kConstruct = 0x42;
constexpr u8 kCallSuper = 0x45;
constexpr u8 kCallProperty = 0x46;
constexpr u8 kReturnVoid = 0x47;
constexpr u8 kReturnValue = 0x48;
constexpr u8 kConstructSuper = 0x49;
constexpr u8 kConstructProp = 0x4a;
constexpr u8 kCallPropLex = 0x4c;
constexpr u8 kCallSuperVoid = 0x4e;
constexpr u8 kCallPropVoid = 0x4f;
constexpr u8 kApplyType = 0x53;
constexpr u8 kNewObject = 0x55;
constexpr u8 kNewArray = 0x56;
constexpr u8 kNewActivation = 0x57;
constexpr u8 kNewClass = 0x58;
constexpr u8 kNewCatch = 0x5a;
constexpr u8 kFindPropStrict = 0x5d;
constexpr u8 kFindProperty = 0x5e;
constexpr u8 kGetLex = 0x60;
constexpr u8 kSetProperty = 0x61;
constexpr u8 kGetLocal = 0x62;
constexpr u8 kSetLocal = 0x63;
constexpr u8 kGetGlobalScope = 0x64;
constexpr u8 kGetScopeObject = 0x65;
constexpr u8 kGetProperty = 0x66;
constexpr u8 kInitProperty = 0x68;
constexpr u8 kDeleteProperty = 0x6a;
constexpr u8 kGetSlot = 0x6c;
constexpr u8 kSetSlot = 0x6d;
constexpr u8 kConvertS = 0x70;
constexpr u8 kConvertI = 0x73;
constexpr u8 kConvertU = 0x74;
constexpr u8 kConvertD = 0x75;
constexpr u8 kConvertB = 0x76;
constexpr u8 kConvertO = 0x77;
constexpr u8 kCheckFilter = 0x78;
constexpr u8 kCoerce = 0x80;
constexpr u8 kCoerceA = 0x82;
constexpr u8 kCoerceS = 0x85;
constexpr u8 kAsType = 0x86;
constexpr u8 kAsTypeLate = 0x87;
constexpr u8 kNegate = 0x90;
constexpr u8 kIncrement = 0x91;
constexpr u8 kIncLocal = 0x92;
constexpr u8 kDecrement = 0x93;
constexpr u8 kDecLocal = 0x94;
constexpr u8 kTypeOf = 0x95;
constexpr u8 kNot = 0x96;
constexpr u8 kBitNot = 0x97;
constexpr u8 kAdd = 0xa0;
constexpr u8 kSubtract = 0xa1;
constexpr u8 kMultiply = 0xa2;
constexpr u8 kDivide = 0xa3;
constexpr u8 kModulo = 0xa4;
constexpr u8 kLshift = 0xa5;
constexpr u8 kRshift = 0xa6;
constexpr u8 kUrshift = 0xa7;
constexpr u8 kBitAnd = 0xa8;
constexpr u8 kBitOr = 0xa9;
constexpr u8 kBitXor = 0xaa;
constexpr u8 kEquals = 0xab;
constexpr u8 kStrictEquals = 0xac;
constexpr u8 kLessThan = 0xad;
constexpr u8 kLessEquals = 0xae;
constexpr u8 kGreaterThan = 0xaf;
constexpr u8 kGreaterEquals = 0xb0;
constexpr u8 kInstanceOf = 0xb1;
constexpr u8 kIsType = 0xb2;
constexpr u8 kIsTypeLate = 0xb3;
constexpr u8 kIn = 0xb4;
constexpr u8 kIncrementI = 0xc0;
constexpr u8 kDecrementI = 0xc1;
constexpr u8 kIncLocalI = 0xc2;
constexpr u8 kDecLocalI = 0xc3;
constexpr u8 kNegateI = 0xc4;
constexpr u8 kAddI = 0xc5;
constexpr u8 kSubtractI = 0xc6;
constexpr u8 kMultiplyI = 0xc7;
constexpr u8 kGetLocal0 = 0xd0;
constexpr u8 kSetLocal0 = 0xd4;
constexpr u8 kDebug = 0xef;
constexpr u8 kDebugLine = 0xf0;
constexpr u8 kDebugFile = 0xf1;
}  // namespace op

// The last segment of a qualified name: `getproperty` names a member, and the
// pool spells it "package.Class.member" when the compiler qualified it.
base::StringRef LastSegment(base::StringRef name) {
  for (mem_size i = name.size(); i-- > 0;) {
    if (name[i] == '.' || name[i] == ':')
      return name.subslice(i + 1, name.size() - i - 1);
  }
  return name;
}

}  // namespace

As3Value As3Value::Null() {
  As3Value v;
  v.type_ = Type::kNull;
  return v;
}
As3Value As3Value::Bool(bool b) {
  As3Value v;
  v.type_ = Type::kBool;
  v.bool_ = b;
  return v;
}
As3Value As3Value::Number(f64 n) {
  As3Value v;
  v.type_ = Type::kNumber;
  v.number_ = n;
  return v;
}
As3Value As3Value::Str(base::StringRef s) {
  As3Value v;
  v.type_ = Type::kString;
  v.string_ = base::String(s);
  return v;
}
As3Value As3Value::Obj(u32 index) {
  As3Value v;
  v.type_ = Type::kObject;
  v.object_ = index;
  return v;
}

struct Avm2::Frame {
  As3Value self;
  base::Vector<As3Value> stack;
  base::Vector<As3Value> locals;
  base::Vector<As3Value> scopes;
  As3Value result;
  bool returned = false;
  u32 unit = 0;

  As3Value Pop() {
    if (stack.empty())
      return As3Value::Undefined();  // an underflowing method gets undefined
    As3Value v = base::move(stack[stack.size() - 1]);
    stack.pop_back();
    return v;
  }
  void Push(As3Value v) { stack.push_back(base::move(v)); }
  As3Value Local(u32 i) const {
    return i < locals.size() ? locals[i] : As3Value::Undefined();
  }
  void SetLocal(u32 i, As3Value v) {
    if (i >= locals.size())
      locals.resize(i + 1);
    locals[i] = base::move(v);
  }
};

namespace {

// Array.push, which is how a menu fills a list: `entryList.push({text: "$NEW"})`.
As3Value ArrayPush(Avm2& vm, const As3Value& self, const base::Vector<As3Value>& args) {
  f64 length = vm.ToNumber(vm.GetProperty(self, "length"));
  if (std::isnan(length) || length < 0)
    length = 0;
  for (const As3Value& arg : args)
    vm.SetProperty(self, base::Format("{}", static_cast<i64>(length++)), arg);
  vm.SetProperty(self, "length", As3Value::Number(length));
  return As3Value::Number(length);
}

// The player's own display API. None of it lives in the movie: a menu calls
// `addFrameScript`, `gotoAndStop` and `addEventListener` on objects Flash
// provides, and a machine without them watches a screen's whole state machine
// resolve to undefined. This is the AVM2 twin of Stage::InstallClipApi.
As3Value DisplayNoOp(Avm2&, const As3Value&, const base::Vector<As3Value>&) {
  return As3Value::Undefined();
}

// addFrameScript(index, fn, ...): the frame handlers a timeline class registers
// in its constructor. Kept by frame so gotoAndStop can run the right one.
As3Value AddFrameScript(Avm2& vm, const As3Value& self,
                        const base::Vector<As3Value>& args) {
  for (mem_size i = 0; i + 1 < args.size(); i += 2) {
    vm.SetProperty(self, base::Format("__frame{}", static_cast<i64>(vm.ToNumber(args[i]))),
                   args[i + 1]);
  }
  return As3Value::Undefined();
}

// gotoAndStop / gotoAndPlay: move to a frame and run what was registered for
// it, which is how an AS3 menu switches panels.
As3Value GotoFrame(Avm2& vm, const As3Value& self, const base::Vector<As3Value>& args) {
  if (args.empty())
    return As3Value::Undefined();
  const f64 frame = vm.ToNumber(args[0]);
  vm.SetProperty(self, "currentFrame", args[0]);
  if (std::isnan(frame))
    return As3Value::Undefined();  // a label; nothing maps labels to frames here
  const As3Value script =
      vm.GetProperty(self, base::Format("__frame{}", static_cast<i64>(frame) - 1));
  vm.Call(script, self, base::Vector<As3Value>());
  return As3Value::Undefined();
}

// addEventListener(type, handler) and dispatchEvent(event), which is how a
// menu's own components talk to each other.
As3Value AddEventListener(Avm2& vm, const As3Value& self,
                          const base::Vector<As3Value>& args) {
  if (args.size() < 2)
    return As3Value::Undefined();
  vm.SetProperty(self, base::Format("__on_{}", vm.ToString(args[0])), args[1]);
  return As3Value::Undefined();
}

As3Value DispatchEvent(Avm2& vm, const As3Value& self,
                       const base::Vector<As3Value>& args) {
  if (args.empty())
    return As3Value::Bool(false);
  const base::String type = vm.ToString(vm.GetProperty(args[0], "type"));
  const As3Value handler = vm.GetProperty(self, base::Format("__on_{}", type));
  base::Vector<As3Value> event;
  event.push_back(args[0]);
  vm.Call(handler, self, event);
  return As3Value::Bool(true);
}

// addChild(child): the display list. A child a menu adds by hand is reachable
// by its own `name`, the way the timeline's children are.
As3Value AddChild(Avm2& vm, const As3Value& self, const base::Vector<As3Value>& args) {
  if (args.empty())
    return As3Value::Undefined();
  const base::String name = vm.ToString(vm.GetProperty(args[0], "name"));
  if (!name.empty() && name != "undefined")
    vm.SetProperty(self, name, args[0]);
  return args[0];
}

}  // namespace

Avm2::Avm2() {
  objects_.push_back(As3Object{});  // index 0 is "no object"
  global_ = NewObject();
  // Everything inherits `push`: a list's entries arrive through it, and the
  // objects they arrive on are the stand-ins below rather than real arrays.
  object_prototype_ = NewObject();
  const As3Value proto = As3Value::Obj(object_prototype_);
  SetProperty(proto, "push", As3Value::Obj(NewNative(&ArrayPush)));
  SetProperty(proto, "addFrameScript", As3Value::Obj(NewNative(&AddFrameScript)));
  SetProperty(proto, "gotoAndStop", As3Value::Obj(NewNative(&GotoFrame)));
  SetProperty(proto, "gotoAndPlay", As3Value::Obj(NewNative(&GotoFrame)));
  SetProperty(proto, "addEventListener", As3Value::Obj(NewNative(&AddEventListener)));
  SetProperty(proto, "dispatchEvent", As3Value::Obj(NewNative(&DispatchEvent)));
  SetProperty(proto, "addChild", As3Value::Obj(NewNative(&AddChild)));
  SetProperty(proto, "addChildAt", As3Value::Obj(NewNative(&AddChild)));
  // Defined so a call does not vanish, but there is nothing here to draw into
  // or to schedule: the translation owns the pixels and the host owns time.
  for (const char* name :
       {"stop", "play", "nextFrame", "prevFrame", "removeEventListener",
        "removeChild", "beginFill", "endFill", "drawRect", "drawRoundRect",
        "lineTo", "moveTo", "curveTo", "clear", "lineStyle", "start",
        "getChildIndex", "setChildIndex", "setTextAutoSize", "setVerticalAlign",
        "removeChildAt", "swapChildren", "hitTestObject"}) {
    SetProperty(proto, name, As3Value::Obj(NewNative(&DisplayNoOp)));
  }
}

u32 Avm2::NewObject(u32 prototype) {
  As3Object object;
  object.prototype = prototype != 0 ? prototype : object_prototype_;
  objects_.push_back(base::move(object));
  return static_cast<u32>(objects_.size() - 1);
}

u32 Avm2::NewArray() {
  const u32 index = NewObject();
  objects_[index].is_array = true;
  SetProperty(As3Value::Obj(index), "length", As3Value::Number(0));
  return index;
}

u32 Avm2::NewNative(As3Native fn) {
  const u32 index = NewObject();
  objects_[index].native = fn;
  objects_[index].is_function = true;
  return index;
}

bool Avm2::ToBool(const As3Value& v) const {
  switch (v.type()) {
    case As3Value::Type::kUndefined:
    case As3Value::Type::kNull:
      return false;
    case As3Value::Type::kBool:
      return v.raw_bool();
    case As3Value::Type::kNumber:
      return v.raw_number() != 0 && !std::isnan(v.raw_number());
    case As3Value::Type::kString:
      return !v.string().empty();
    case As3Value::Type::kObject:
      return true;
  }
  return false;
}

f64 Avm2::ToNumber(const As3Value& v) const {
  switch (v.type()) {
    case As3Value::Type::kBool:
      return v.raw_bool() ? 1.0 : 0.0;
    case As3Value::Type::kNumber:
      return v.raw_number();
    case As3Value::Type::kString: {
      const base::String& s = v.string();
      if (s.empty())
        return 0.0;
      char* end = nullptr;
      const f64 parsed = std::strtod(s.c_str(), &end);
      return end && *end == '\0' ? parsed : NAN;
    }
    case As3Value::Type::kNull:
      return 0.0;
    default:
      return NAN;
  }
}

base::String Avm2::ToString(const As3Value& v) {
  switch (v.type()) {
    case As3Value::Type::kUndefined:
      return "undefined";
    case As3Value::Type::kNull:
      return "null";
    case As3Value::Type::kBool:
      return v.raw_bool() ? "true" : "false";
    case As3Value::Type::kString:
      return v.string();
    case As3Value::Type::kNumber: {
      const f64 n = v.raw_number();
      if (std::isnan(n))
        return "NaN";
      if (n == static_cast<i64>(n))
        return base::Format("{}", static_cast<i64>(n));
      return base::Format("{}", n);
    }
    case As3Value::Type::kObject:
      return Valid(v.object()) && objects_[v.object()].is_array ? "[object Array]"
                                                                : "[object Object]";
  }
  return {};
}

As3Value Avm2::GetProperty(const As3Value& target, base::StringRef name) {
  if (!target.is_object() || !Valid(target.object()))
    return As3Value::Undefined();
  const base::String key(LastSegment(name));
  for (u32 current = target.object(), guard = 0; guard < 64 && Valid(current); ++guard) {
    if (const As3Value* found = objects_[current].props.find(key))
      return *found;
    current = objects_[current].prototype;
  }
  // A stand-in's missing member is another stand-in: the player would have put
  // a display object there, and the code walks straight through it.
  if (objects_[target.object()].is_display && objects_.size() < kMaxObjects) {
    const u32 child = NewDisplay();
    SetProperty(target, key, As3Value::Obj(child));
    return As3Value::Obj(child);
  }
  return As3Value::Undefined();
}

void Avm2::SetProperty(const As3Value& target, base::StringRef name,
                       const As3Value& value) {
  if (!target.is_object() || !Valid(target.object()))
    return;
  const base::String key(LastSegment(name));
  As3Object& object = objects_[target.object()];
  if (object.props.find(key) == nullptr)
    object.order.push_back(key);
  object.props[key] = value;
}

void Avm2::AddAbc(const AbcFile& abc) {
  Unit unit;
  unit.abc = &abc;
  units_.push_back(base::move(unit));
  const u32 index = static_cast<u32>(units_.size() - 1);
  for (u32 c = 0; c < abc.classes.size(); ++c)
    InstallClass(index, c);
}

void Avm2::InstallClass(u32 unit, u32 class_index) {
  const AbcFile& abc = *units_[unit].abc;
  const AbcClass& klass = abc.classes[class_index];
  if (klass.name.empty())
    return;
  const u32 object = NewObject();
  objects_[object].is_class = true;
  objects_[object].klass = static_cast<i32>(class_index);
  objects_[object].unit = unit;
  // A class's static methods live on the class itself, and the menus lean on
  // them: `Shared.GlobalFunc.MaintainTextFormat` and its neighbours are how a
  // screen formats its own text.
  for (const AbcTrait& trait : klass.static_traits) {
    if (trait.kind != TraitKind::kMethod && trait.kind != TraitKind::kGetter)
      continue;
    if (trait.method >= abc.methods.size())
      continue;
    const u32 fn = NewObject();
    objects_[fn].is_function = true;
    objects_[fn].method = trait.method;
    objects_[fn].unit = unit;
    SetProperty(As3Value::Obj(object), trait.name, As3Value::Obj(fn));
  }
  classes_[klass.name] = object;
  // Reachable by qualified name and by the bare class name, since a script
  // names it either way depending on its imports.
  SetProperty(As3Value::Obj(global_), klass.name, As3Value::Obj(object));
  SetProperty(As3Value::Obj(global_), LastSegment(klass.name), As3Value::Obj(object));
}

u32 Avm2::ClassObject(u32 unit, base::StringRef name) {
  (void)unit;
  if (const u32* found = classes_.find(base::String(name)))
    return *found;
  // A bare name: take the first class whose last segment matches.
  const base::StringRef bare = LastSegment(name);
  for (const auto& entry : classes_) {
    if (LastSegment(entry.key) == bare)
      return entry.value;
  }
  return 0;
}

// A class's declared members, as the player would have them before the
// constructor runs. Flash puts the display objects a timeline placed on the
// instance first - a panel's `List_mc` is there because the frame placed it,
// not because the code made it - so a machine that starts with a bare object
// watches every assignment to one go nowhere.
// A stand-in for a display object. It answers to `visible` from the start: the
// player's every display object has one, and a timeline script says how its
// clip opens by assigning to it. `findproperty` walks the scope chain for an
// object that already carries the name and falls back to the global when none
// does, so a stand-in without `visible` sends its clip's own state to the
// global object and the clip reads as showing whatever it was exported as.
u32 Avm2::NewDisplay(u32 prototype) {
  const u32 object = NewObject(prototype);
  objects_[object].is_display = true;
  SetProperty(As3Value::Obj(object), "visible", As3Value::Bool(true));
  return object;
}

u32 Avm2::MethodsOf(base::StringRef class_name, u32 depth) {
  const base::String key(class_name);
  if (const u32* found = methods_.find(key))
    return *found;
  const u32 klass = ClassObject(0, class_name);
  if (!Valid(klass) || depth > 16)
    return 0;
  const u32 unit = objects_[klass].unit;
  const i32 index = objects_[klass].klass;
  if (unit >= units_.size() || index < 0)
    return 0;
  const AbcFile& abc = *units_[unit].abc;
  if (static_cast<u32>(index) >= abc.classes.size())
    return 0;
  const AbcClass& definition = abc.classes[index];

  const u32 object = NewObject(MethodsOf(definition.super, depth + 1));
  methods_[key] = object;  // recorded before filling, so a cycle stops here
  for (const AbcTrait& trait : definition.instance_traits) {
    if (trait.kind != TraitKind::kMethod && trait.kind != TraitKind::kGetter)
      continue;
    if (trait.method >= abc.methods.size())
      continue;
    const u32 fn = NewObject();
    objects_[fn].is_function = true;
    objects_[fn].method = trait.method;
    objects_[fn].unit = unit;
    SetProperty(As3Value::Obj(object), trait.name, As3Value::Obj(fn));
  }
  return object;
}

void Avm2::InstallTraits(u32 unit, const AbcClass& definition,
                        const As3Value& instance) {
  // Every display object has one, and a menu draws its own backing plates
  // through it. The drawing goes nowhere here, but the calls have to land.
  const u32 graphics = NewDisplay();
  SetProperty(instance, "graphics", As3Value::Obj(graphics));
  // The instance is a display object too, and every one of those is visible
  // until something says otherwise (see NewDisplay).
  SetProperty(instance, "visible", As3Value::Bool(true));
  for (const AbcTrait& trait : definition.instance_traits) {
    // A method is callable by name, which is how the host reaches a screen.
    if (trait.kind == TraitKind::kMethod || trait.kind == TraitKind::kGetter) {
      const u32 fn = NewObject();
      objects_[fn].is_function = true;
      objects_[fn].method = trait.method;
      objects_[fn].unit = unit;
      SetProperty(instance, trait.name, As3Value::Obj(fn));
      continue;
    }
    if (trait.kind != TraitKind::kSlot && trait.kind != TraitKind::kConst)
      continue;
    if (trait.name.empty())
      continue;
    const base::StringRef type = LastSegment(trait.type);
    if (type == "Number" || type == "int" || type == "uint" || type == "String" ||
        type == "Boolean" || type == "*" || type.empty())
      continue;  // a value, which the constructor assigns for itself
    // The stand-in stands for something of a declared type, so it answers to
    // that type's methods: `List_mc:MainMenuList` has MainMenuList's.
    const u32 child = NewDisplay(MethodsOf(trait.type, 0));
    SetProperty(instance, trait.name, As3Value::Obj(child));
    // A nested panel is a timeline class of its own, and its frame-1 script is
    // what says how it opens. Every one of Fallout 4's ships hidden that way,
    // so a stand-in that never runs it reads as a panel that is showing.
    RunOpeningFrame(trait.type, As3Value::Obj(child));
  }
}

// The two kinds of method the player runs that nothing inside the class calls.
//
// A timeline class keeps its frame code in methods the constructor registers
// with `addFrameScript`, and the player runs frame 1's when the frame is shown.
// Beside those, Flash generates a `__setProp_<child>_<...>` for every component
// whose parameters were set in the authoring tool, and the frame script calls
// it. That generated method is where a panel says what its list is made of - it
// is the AS3 spelling of the `construct` clip-event handler an AS2 movie hangs
// on a placement - so a machine that runs only the constructor sees a class
// that does almost nothing.
// The opening frame code of the class `class_name`, run against `instance`.
void Avm2::RunOpeningFrame(base::StringRef class_name, const As3Value& instance) {
  const u32 klass = ClassObject(0, class_name);
  if (!Valid(klass))
    return;
  const u32 unit = objects_[klass].unit;
  const i32 index = objects_[klass].klass;
  if (unit >= units_.size() || index < 0)
    return;
  const AbcFile& abc = *units_[unit].abc;
  if (static_cast<u32>(index) >= abc.classes.size())
    return;
  RunFrameScripts(unit, abc.classes[index], instance);
}

void Avm2::RunFrameScripts(u32 unit, const AbcClass& definition,
                           const As3Value& instance) {
  const AbcFile& abc = *units_[unit].abc;
  for (const AbcTrait& trait : definition.instance_traits) {
    if (trait.kind != TraitKind::kMethod || trait.method >= abc.methods.size())
      continue;
    const base::StringRef name = LastSegment(trait.name);
    // Only the frame the clip opens on. A panel keeps a script on each of its
    // frames and they contradict each other by design - Fallout 4's ship
    // `frame1: visible = false`, `frame3: visible = true`, `frame5: visible =
    // false` - so running all three leaves the clip in whichever state the
    // traits happen to end on rather than the one it starts in.
    const bool frame = name == base::StringRef("frame1");
    const bool props =
        name.size() > 9 && name.subslice(0, 9) == base::StringRef("__setProp");
    if (!frame && !props)
      continue;
    base::Vector<As3Value> args;
    Run(unit, trait.method, instance, args, 1);
  }
}

As3Value Avm2::Construct(base::StringRef class_name) {
  const u32 klass = ClassObject(0, class_name);
  if (!Valid(klass))
    return As3Value::Undefined();
  const u32 unit = objects_[klass].unit;
  if (unit >= units_.size())
    return As3Value::Undefined();
  const AbcFile& abc = *units_[unit].abc;
  const i32 index = objects_[klass].klass;
  if (index < 0 || static_cast<u32>(index) >= abc.classes.size())
    return As3Value::Undefined();
  const AbcClass& definition = abc.classes[index];

  const u32 instance = NewObject();
  SetProperty(As3Value::Obj(instance), "constructor", As3Value::Obj(klass));
  // The instance starts with the slots its traits declare, so a `setslot` in
  // the constructor lands somewhere rather than off the end.
  objects_[instance].slots.resize(definition.instance_traits.size() + 1);
  InstallTraits(unit, definition, As3Value::Obj(instance));
  base::Vector<As3Value> args;
  Run(unit, definition.constructor, As3Value::Obj(instance), args, 0);
  RunFrameScripts(unit, definition, As3Value::Obj(instance));
  return As3Value::Obj(instance);
}

bool Avm2::Invoke(const As3Value& instance, base::StringRef name,
                  const base::Vector<As3Value>& args) {
  const As3Value fn = GetProperty(instance, name);
  if (!fn.is_object() || !Valid(fn.object()) || !objects_[fn.object()].is_function)
    return false;
  Call(fn, instance, args);
  return true;
}

As3Value Avm2::Call(const As3Value& function, const As3Value& self,
                    const base::Vector<As3Value>& args) {
  if (!function.is_object() || !Valid(function.object()))
    return As3Value::Undefined();
  const As3Object& fn = objects_[function.object()];
  if (fn.native)
    return fn.native(*this, self, args);
  if (!fn.is_function || fn.method == ~0u)
    return As3Value::Undefined();
  return Run(fn.unit, fn.method, self, args, 0);
}

As3Value Avm2::FindProperty(Frame& frame, base::StringRef name, bool strict) {
  const base::String key(LastSegment(name));
  for (mem_size i = frame.scopes.size(); i-- > 0;) {
    const As3Value& scope = frame.scopes[i];
    if (!scope.is_object() || !Valid(scope.object()))
      continue;
    for (u32 current = scope.object(), guard = 0; guard < 64 && Valid(current); ++guard) {
      if (objects_[current].props.find(key))
        return scope;
      current = objects_[current].prototype;
    }
  }
  if (GetProperty(As3Value::Obj(global_), key).is_undefined() && strict)
    return As3Value::Obj(global_);  // strict throws in the player; here it is global
  return As3Value::Obj(global_);
}

As3Value Avm2::Run(u32 unit, u32 method_index, const As3Value& self,
                   const base::Vector<As3Value>& args, u32 depth) {
  // The machine's own depth, not the caller's guess at it: a method reached
  // through `callproperty` is one level deeper than the one that called it, and
  // counting from the entry point every time lets a cycle run until the C++
  // stack gives out instead of the guard catching it.
  (void)depth;
  if (unit >= units_.size() || depth_ > kMaxDepth)
    return As3Value::Undefined();
  const AbcFile& abc = *units_[unit].abc;
  if (method_index >= abc.methods.size())
    return As3Value::Undefined();
  const AbcMethod& method = abc.methods[method_index];
  if (method.body >= abc.bodies.size())
    return As3Value::Undefined();
  const AbcMethodBody& body = abc.bodies[method.body];

  const u64 key = (static_cast<u64>(unit) << 32) | method_index;
  if (decoded_.find(key) == nullptr) {
    decoded_[key] =
        base::MakeUnique<base::Vector<AbcInstruction>>(DisassembleMethod(body));
  }
  const base::Vector<AbcInstruction>& code = *decoded_[key];

  Frame frame;
  frame.unit = unit;
  frame.self = self;
  frame.locals.resize(base::Max<mem_size>(body.local_count, args.size() + 1));
  frame.SetLocal(0, self);
  for (mem_size i = 0; i < args.size(); ++i)
    frame.SetLocal(static_cast<u32>(i + 1), args[i]);

  // Offset -> instruction index, so a branch lands on an instruction rather
  // than in the middle of one.
  base::UnorderedMap<u32, u32> at;
  for (u32 i = 0; i < code.size(); ++i)
    at[code[i].offset] = i;

  auto name_of = [&abc](u32 index) -> base::StringRef {
    return index < abc.names.size() ? base::StringRef(abc.names[index])
                                    : base::StringRef();
  };
  auto branch = [&](const AbcInstruction& insn, u32& i) {
    const u32 target = static_cast<u32>(static_cast<i32>(insn.end) + insn.jump);
    if (const u32* found = at.find(target))
      i = *found;
    else
      i = static_cast<u32>(code.size());
  };

  ++depth_;
  struct DepthGuard {
    u32& depth;
    ~DepthGuard() { --depth; }
  } guard{depth_};

  for (u32 i = 0; i < code.size();) {
    if (++steps_ > kMaxSteps) {
      exhausted_ = true;
      return As3Value::Undefined();
    }
    if (frame.returned)
      break;
    const AbcInstruction& insn = code[i];
    u32 next = i + 1;
    switch (insn.op) {
      case op::kNop:
      case op::kLabel:
      case op::kDebug:
      case op::kDebugLine:
      case op::kDebugFile:
      case op::kCoerceA:
      case op::kCheckFilter:
        break;

      // --- the stack ---
      case op::kPushByte:
        frame.Push(As3Value::Number(static_cast<f64>(static_cast<i8>(insn.a))));
        break;
      case op::kPushShort:
        frame.Push(As3Value::Number(static_cast<f64>(static_cast<i32>(insn.a))));
        break;
      case op::kPushTrue:
        frame.Push(As3Value::Bool(true));
        break;
      case op::kPushFalse:
        frame.Push(As3Value::Bool(false));
        break;
      case op::kPushNan:
        frame.Push(As3Value::Number(NAN));
        break;
      case 0x20:  // pushnull
        frame.Push(As3Value::Null());
        break;
      case 0x21:  // pushundefined
        frame.Push(As3Value::Undefined());
        break;
      case op::kPushString:
        frame.Push(As3Value::Str(insn.a < abc.strings.size() ? base::StringRef(abc.strings[insn.a])
                                                             : base::StringRef()));
        break;
      case op::kPushInt:
      case op::kPushUint:
        frame.Push(As3Value::Number(
            insn.a < abc.ints.size() ? static_cast<f64>(abc.ints[insn.a]) : 0.0));
        break;
      case op::kPushDouble:
        frame.Push(As3Value::Number(insn.a < abc.doubles.size() ? abc.doubles[insn.a] : 0.0));
        break;
      case op::kPop:
        frame.Pop();
        break;
      case op::kDup: {
        const As3Value v = frame.Pop();
        frame.Push(v);
        frame.Push(v);
        break;
      }
      case op::kSwap: {
        const As3Value a = frame.Pop();
        const As3Value b = frame.Pop();
        frame.Push(a);
        frame.Push(b);
        break;
      }

      // --- locals and scopes ---
      case op::kGetLocal:
        frame.Push(frame.Local(insn.a));
        break;
      case op::kSetLocal:
        frame.SetLocal(insn.a, frame.Pop());
        break;
      case op::kKill:
        frame.SetLocal(insn.a, As3Value::Undefined());
        break;
      case op::kGetLocal0:
      case op::kGetLocal0 + 1:
      case op::kGetLocal0 + 2:
      case op::kGetLocal0 + 3:
        frame.Push(frame.Local(insn.op - op::kGetLocal0));
        break;
      case op::kSetLocal0:
      case op::kSetLocal0 + 1:
      case op::kSetLocal0 + 2:
      case op::kSetLocal0 + 3:
        frame.SetLocal(insn.op - op::kSetLocal0, frame.Pop());
        break;
      case op::kPushScope:
      case op::kPushWith:
        frame.scopes.push_back(frame.Pop());
        break;
      case op::kPopScope:
        if (!frame.scopes.empty())
          frame.scopes.pop_back();
        break;
      case op::kGetScopeObject:
        frame.Push(insn.a < frame.scopes.size() ? frame.scopes[insn.a]
                                                : As3Value::Undefined());
        break;
      case op::kGetGlobalScope:
        frame.Push(As3Value::Obj(global_));
        break;
      case op::kNewActivation:
        frame.Push(As3Value::Obj(NewObject()));
        break;

      // --- properties ---
      case op::kGetLex: {
        const base::StringRef name = name_of(insn.a);
        const As3Value holder = FindProperty(frame, name, true);
        frame.Push(GetProperty(holder, name));
        break;
      }
      case op::kFindPropStrict:
      case op::kFindProperty:
        frame.Push(FindProperty(frame, name_of(insn.a), insn.op == op::kFindPropStrict));
        break;
      case op::kGetProperty: {
        const As3Value target = frame.Pop();
        frame.Push(GetProperty(target, name_of(insn.a)));
        break;
      }
      case op::kSetProperty:
      case op::kInitProperty: {
        const As3Value value = frame.Pop();
        const As3Value target = frame.Pop();
        SetProperty(target, name_of(insn.a), value);
        break;
      }
      case op::kDeleteProperty: {
        const As3Value target = frame.Pop();
        if (target.is_object() && Valid(target.object()))
          objects_[target.object()].props.erase(base::String(LastSegment(name_of(insn.a))));
        frame.Push(As3Value::Bool(true));
        break;
      }
      case op::kGetSlot: {
        const As3Value target = frame.Pop();
        if (target.is_object() && Valid(target.object()) &&
            insn.a < objects_[target.object()].slots.size())
          frame.Push(objects_[target.object()].slots[insn.a]);
        else
          frame.Push(As3Value::Undefined());
        break;
      }
      case op::kSetSlot: {
        const As3Value value = frame.Pop();
        const As3Value target = frame.Pop();
        if (target.is_object() && Valid(target.object())) {
          As3Object& object = objects_[target.object()];
          if (insn.a >= object.slots.size())
            object.slots.resize(insn.a + 1);
          object.slots[insn.a] = value;
        }
        break;
      }
      case op::kGetSuper: {
        const As3Value target = frame.Pop();
        frame.Push(GetProperty(target, name_of(insn.a)));
        break;
      }
      case op::kSetSuper: {
        const As3Value value = frame.Pop();
        const As3Value target = frame.Pop();
        SetProperty(target, name_of(insn.a), value);
        break;
      }

      // --- calls ---
      case op::kCallProperty:
      case op::kCallPropLex:
      case op::kCallPropVoid:
      case op::kCallSuper:
      case op::kCallSuperVoid: {
        base::Vector<As3Value> call_args;
        call_args.resize(insn.b);
        for (u32 k = insn.b; k-- > 0;)
          call_args[k] = frame.Pop();
        const As3Value target = frame.Pop();
        const As3Value fn = GetProperty(target, name_of(insn.a));
        if (!fn.is_object() || !Valid(fn.object()) ||
            !objects_[fn.object()].is_function) {
          const base::String key(LastSegment(name_of(insn.a)));
          if (u32* seen = unresolved_.find(key))
            ++*seen;
          else
            unresolved_[key] = 1;
        }
        const As3Value result = Call(fn, target, call_args);
        if (insn.op != op::kCallPropVoid && insn.op != op::kCallSuperVoid)
          frame.Push(result);
        break;
      }
      case op::kCall: {
        base::Vector<As3Value> call_args;
        call_args.resize(insn.a);
        for (u32 k = insn.a; k-- > 0;)
          call_args[k] = frame.Pop();
        const As3Value receiver = frame.Pop();
        const As3Value fn = frame.Pop();
        frame.Push(Call(fn, receiver, call_args));
        break;
      }
      case op::kConstructProp:
      case op::kConstruct: {
        const u32 argc = insn.op == op::kConstruct ? insn.a : insn.b;
        base::Vector<As3Value> call_args;
        call_args.resize(argc);
        for (u32 k = argc; k-- > 0;)
          call_args[k] = frame.Pop();
        const As3Value target = frame.Pop();
        const As3Value klass = insn.op == op::kConstruct
                                   ? target
                                   : GetProperty(target, name_of(insn.a));
        // A class object builds an instance and runs its constructor; anything
        // else yields a plain object, which is what an unknown type comes to.
        const u32 instance = NewObject();
        if (klass.is_object() && Valid(klass.object()) && objects_[klass.object()].is_class) {
          SetProperty(As3Value::Obj(instance), "constructor", klass);
          const u32 klass_unit = objects_[klass.object()].unit;
          const i32 index = objects_[klass.object()].klass;
          if (klass_unit < units_.size() && index >= 0) {
            const AbcFile& owner = *units_[klass_unit].abc;
            if (static_cast<u32>(index) < owner.classes.size()) {
              objects_[instance].slots.resize(
                  owner.classes[index].instance_traits.size() + 1);
              InstallTraits(klass_unit, owner.classes[index], As3Value::Obj(instance));
              Run(klass_unit, owner.classes[index].constructor, As3Value::Obj(instance),
                  call_args, depth + 1);
            }
          }
        }
        frame.Push(As3Value::Obj(instance));
        break;
      }
      case op::kConstructSuper: {
        base::Vector<As3Value> call_args;
        call_args.resize(insn.a);
        for (u32 k = insn.a; k-- > 0;)
          call_args[k] = frame.Pop();
        frame.Pop();  // the object; the base constructor has nothing to add yet
        break;
      }
      case op::kNewFunction: {
        const u32 fn = NewObject();
        objects_[fn].is_function = true;
        objects_[fn].method = insn.a;
        objects_[fn].unit = unit;
        frame.Push(As3Value::Obj(fn));
        break;
      }
      case op::kNewClass: {
        const As3Value base = frame.Pop();
        (void)base;
        frame.Push(As3Value::Obj(NewObject()));
        break;
      }
      case op::kNewObject: {
        const u32 object = NewObject();
        for (u32 k = 0; k < insn.a; ++k) {
          const As3Value value = frame.Pop();
          const As3Value name = frame.Pop();
          SetProperty(As3Value::Obj(object), ToString(name), value);
        }
        frame.Push(As3Value::Obj(object));
        break;
      }
      case op::kNewArray: {
        const u32 array = NewArray();
        for (u32 k = insn.a; k-- > 0;)
          SetProperty(As3Value::Obj(array), base::Format("{}", k), frame.Pop());
        SetProperty(As3Value::Obj(array), "length", As3Value::Number(insn.a));
        frame.Push(As3Value::Obj(array));
        break;
      }
      case op::kNewCatch:
        frame.Push(As3Value::Obj(NewObject()));
        break;
      case op::kApplyType: {
        // `Vector.<String>`: the type arguments are applied to the factory on
        // the stack. Nothing here checks a type, so the factory stands for the
        // applied one.
        for (u32 k = 0; k < insn.a; ++k)
          frame.Pop();
        break;
      }

      // --- control flow ---
      case op::kJump:
        branch(insn, next);
        break;
      case op::kIfTrue:
        if (ToBool(frame.Pop()))
          branch(insn, next);
        break;
      case op::kIfFalse:
        if (!ToBool(frame.Pop()))
          branch(insn, next);
        break;
      case op::kIfEq:
      case op::kIfNe:
      case op::kIfStrictEq:
      case op::kIfStrictNe: {
        const As3Value b = frame.Pop();
        const As3Value a = frame.Pop();
        bool equal = false;
        if (a.is_object() && b.is_object())
          equal = a.object() == b.object();
        else if (a.is_string() && b.is_string())
          equal = a.string() == b.string();
        else if ((a.is_undefined() || a.is_null()) && (b.is_undefined() || b.is_null()))
          equal = true;
        else if (insn.op == op::kIfStrictEq || insn.op == op::kIfStrictNe)
          equal = a.type() == b.type() && ToNumber(a) == ToNumber(b);
        else
          equal = ToNumber(a) == ToNumber(b);
        const bool want = insn.op == op::kIfEq || insn.op == op::kIfStrictEq;
        if (equal == want)
          branch(insn, next);
        break;
      }
      case op::kIfLt:
      case op::kIfLe:
      case op::kIfGt:
      case op::kIfGe:
      case op::kIfNlt:
      case op::kIfNle:
      case op::kIfNgt:
      case op::kIfNge: {
        const f64 b = ToNumber(frame.Pop());
        const f64 a = ToNumber(frame.Pop());
        bool take = false;
        switch (insn.op) {
          case op::kIfLt: take = a < b; break;
          case op::kIfLe: take = a <= b; break;
          case op::kIfGt: take = a > b; break;
          case op::kIfGe: take = a >= b; break;
          case op::kIfNlt: take = !(a < b); break;
          case op::kIfNle: take = !(a <= b); break;
          case op::kIfNgt: take = !(a > b); break;
          default: take = !(a >= b); break;
        }
        if (take)
          branch(insn, next);
        break;
      }
      case op::kLookupSwitch: {
        const i64 index = static_cast<i64>(ToNumber(frame.Pop()));
        // The default is the last entry; the cases before it are 0..n.
        const mem_size count = insn.cases.size();
        const mem_size pick =
            (index >= 0 && static_cast<mem_size>(index) + 1 < count)
                ? static_cast<mem_size>(index)
                : (count > 0 ? count - 1 : 0);
        if (count > 0) {
          const u32 target =
              static_cast<u32>(static_cast<i32>(insn.offset) + insn.cases[pick]);
          if (const u32* found = at.find(target))
            next = *found;
          else
            next = static_cast<u32>(code.size());
        }
        break;
      }
      case op::kReturnVoid:
        frame.returned = true;
        break;
      case op::kReturnValue:
        frame.result = frame.Pop();
        frame.returned = true;
        break;
      case op::kThrow:
        frame.Pop();  // nothing catches here; the method simply stops
        frame.returned = true;
        break;

      // --- conversions and arithmetic ---
      case op::kConvertS:
      case op::kCoerceS:
        frame.Push(As3Value::Str(ToString(frame.Pop())));
        break;
      case op::kConvertB:
        frame.Push(As3Value::Bool(ToBool(frame.Pop())));
        break;
      case op::kConvertD:
        frame.Push(As3Value::Number(ToNumber(frame.Pop())));
        break;
      case op::kConvertI:
        frame.Push(As3Value::Number(static_cast<f64>(static_cast<i32>(ToNumber(frame.Pop())))));
        break;
      case op::kConvertU:
        frame.Push(As3Value::Number(static_cast<f64>(static_cast<u32>(ToNumber(frame.Pop())))));
        break;
      case op::kConvertO:
      case op::kCoerce:
      case op::kAsType:
      case op::kAsTypeLate:
        break;  // the value keeps its own type; nothing here checks one
      case op::kAdd: {
        const As3Value b = frame.Pop();
        const As3Value a = frame.Pop();
        if (a.is_string() || b.is_string())
          frame.Push(As3Value::Str(ToString(a) + ToString(b)));
        else
          frame.Push(As3Value::Number(ToNumber(a) + ToNumber(b)));
        break;
      }
      case op::kSubtract:
      case op::kMultiply:
      case op::kDivide:
      case op::kModulo:
      case op::kAddI:
      case op::kSubtractI:
      case op::kMultiplyI: {
        const f64 b = ToNumber(frame.Pop());
        const f64 a = ToNumber(frame.Pop());
        f64 result = 0;
        switch (insn.op) {
          case op::kSubtract:
          case op::kSubtractI: result = a - b; break;
          case op::kMultiply:
          case op::kMultiplyI: result = a * b; break;
          case op::kDivide: result = a / b; break;
          case op::kModulo: result = b == 0 ? NAN : std::fmod(a, b); break;
          default: result = a + b; break;
        }
        frame.Push(As3Value::Number(result));
        break;
      }
      case op::kLshift:
      case op::kRshift:
      case op::kUrshift:
      case op::kBitAnd:
      case op::kBitOr:
      case op::kBitXor: {
        const i32 b = static_cast<i32>(ToNumber(frame.Pop()));
        const i32 a = static_cast<i32>(ToNumber(frame.Pop()));
        i64 result = 0;
        switch (insn.op) {
          case op::kLshift: result = static_cast<i64>(a) << (b & 31); break;
          case op::kRshift: result = a >> (b & 31); break;
          case op::kUrshift: result = static_cast<u32>(a) >> (b & 31); break;
          case op::kBitAnd: result = a & b; break;
          case op::kBitOr: result = a | b; break;
          default: result = a ^ b; break;
        }
        frame.Push(As3Value::Number(static_cast<f64>(result)));
        break;
      }
      case op::kNegate:
      case op::kNegateI:
        frame.Push(As3Value::Number(-ToNumber(frame.Pop())));
        break;
      case op::kBitNot:
        frame.Push(As3Value::Number(static_cast<f64>(~static_cast<i32>(ToNumber(frame.Pop())))));
        break;
      case op::kNot:
        frame.Push(As3Value::Bool(!ToBool(frame.Pop())));
        break;
      case op::kIncrement:
      case op::kIncrementI:
        frame.Push(As3Value::Number(ToNumber(frame.Pop()) + 1));
        break;
      case op::kDecrement:
      case op::kDecrementI:
        frame.Push(As3Value::Number(ToNumber(frame.Pop()) - 1));
        break;
      case op::kIncLocal:
      case op::kIncLocalI:
        frame.SetLocal(insn.a, As3Value::Number(ToNumber(frame.Local(insn.a)) + 1));
        break;
      case op::kDecLocal:
      case op::kDecLocalI:
        frame.SetLocal(insn.a, As3Value::Number(ToNumber(frame.Local(insn.a)) - 1));
        break;
      case op::kEquals:
      case op::kStrictEquals: {
        const As3Value b = frame.Pop();
        const As3Value a = frame.Pop();
        bool equal = false;
        if (a.is_object() && b.is_object())
          equal = a.object() == b.object();
        else if (a.is_string() && b.is_string())
          equal = a.string() == b.string();
        else if ((a.is_undefined() || a.is_null()) && (b.is_undefined() || b.is_null()))
          equal = true;
        else if (insn.op == op::kStrictEquals)
          equal = a.type() == b.type() && ToNumber(a) == ToNumber(b);
        else
          equal = ToNumber(a) == ToNumber(b);
        frame.Push(As3Value::Bool(equal));
        break;
      }
      case op::kLessThan:
      case op::kLessEquals:
      case op::kGreaterThan:
      case op::kGreaterEquals: {
        const f64 b = ToNumber(frame.Pop());
        const f64 a = ToNumber(frame.Pop());
        bool result = false;
        switch (insn.op) {
          case op::kLessThan: result = a < b; break;
          case op::kLessEquals: result = a <= b; break;
          case op::kGreaterThan: result = a > b; break;
          default: result = a >= b; break;
        }
        frame.Push(As3Value::Bool(result));
        break;
      }
      case op::kTypeOf:
        frame.Push(As3Value::Str(ToString(frame.Pop()) == "undefined" ? "undefined"
                                                                      : "object"));
        break;
      case op::kInstanceOf:
      case op::kIsType:
      case op::kIsTypeLate:
      case op::kIn:
        frame.Pop();
        frame.Pop();
        frame.Push(As3Value::Bool(false));
        break;
      // `for (k in o)`. The player keeps the object and a cursor in two locals
      // and walks them with hasnext2; the cursor is 1-based, and running off
      // the end clears the pair so the loop's own test ends it.
      case op::kHasNext2: {
        const As3Value target = frame.Local(insn.a);
        const mem_size cursor = static_cast<mem_size>(ToNumber(frame.Local(insn.b)));
        const bool more = target.is_object() && Valid(target.object()) &&
                          cursor < objects_[target.object()].order.size();
        if (more) {
          frame.SetLocal(insn.b, As3Value::Number(static_cast<f64>(cursor + 1)));
        } else {
          frame.SetLocal(insn.a, As3Value::Null());
          frame.SetLocal(insn.b, As3Value::Number(0));
        }
        frame.Push(As3Value::Bool(more));
        break;
      }
      case op::kNextName:
      case op::kNextValue: {
        const mem_size cursor = static_cast<mem_size>(ToNumber(frame.Pop()));
        const As3Value target = frame.Pop();
        if (!target.is_object() || !Valid(target.object()) || cursor == 0 ||
            cursor > objects_[target.object()].order.size()) {
          frame.Push(As3Value::Undefined());
          break;
        }
        const base::String& key = objects_[target.object()].order[cursor - 1];
        frame.Push(insn.op == op::kNextName ? As3Value::Str(key)
                                            : GetProperty(target, key));
        break;
      }

      default: {
        const base::StringRef name = AbcOpName(insn.op);
        const base::String key = name.empty() ? base::Format("0x{:02x}", insn.op)
                                              : base::String(name);
        u32* seen = unhandled_.find(key);
        if (seen)
          ++*seen;
        else
          unhandled_[key] = 1;
        break;
      }
    }
    i = next;
  }
  return frame.result;
}

}  // namespace rx::swf
