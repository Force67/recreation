#ifndef RECREATION_SWF_VM_H_
#define RECREATION_SWF_VM_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/swf/avm1.h"
#include "core/types.h"

namespace rx::swf {

// An ActionScript 2 interpreter for the bytecode the Bethesda menus are built
// from.
//
// The rest of this module translates a movie statically: it reads the display
// list as the designer left it and emits markup. That is enough to draw a menu
// but not to run one, because a shipped menu is an empty frame until its script
// fills it (see decompile.cc for what that script looks like). Everything the
// player actually sees - the option lists, the selection, the state machine -
// is produced by the code in here being executed.
//
// Scope: the instruction set and the standard-library surface the shipped menus
// use, not all of ActionScript. Objects are owned by the Vm and referenced by
// index, so there is no collector: a menu's script runs, fills a screen and
// stops, and everything goes when the Vm does.

class Vm;

// A runtime value. Numbers are f64 and strings are copied, as in the language.
class AsValue {
 public:
  enum class Type : u8 { kUndefined, kNull, kBool, kNumber, kString, kObject };

  AsValue() = default;
  static AsValue Undefined() { return AsValue(); }
  static AsValue Null();
  static AsValue Bool(bool v);
  static AsValue Number(f64 v);
  static AsValue Str(base::StringRef v);
  static AsValue Obj(u32 index);

  Type type() const { return type_; }
  bool is_undefined() const { return type_ == Type::kUndefined; }
  bool is_object() const { return type_ == Type::kObject; }
  bool is_string() const { return type_ == Type::kString; }
  u32 object() const { return object_; }
  const base::String& string() const { return string_; }
  f64 raw_number() const { return number_; }
  bool raw_bool() const { return bool_; }

 private:
  Type type_ = Type::kUndefined;
  bool bool_ = false;
  f64 number_ = 0;
  base::String string_;
  u32 object_ = 0;
};

// A native implementation, for the standard library and for the host objects a
// menu drives (movie clips, text fields, the game bridge).
using NativeFn = AsValue (*)(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args);

// Where a function's body lives: a half-open range of actions in one of the
// scripts the Vm has loaded, plus the constant pool in force there.
struct AsFunctionBody {
  u32 script = 0;
  u32 first = 0;
  u32 count = 0;
  u16 flags = 0;         // DefineFunction2 preload/suppress bits
  u8 register_count = 0;
  base::Vector<base::String> params;
  base::Vector<u8> param_registers;  // 0 = argument goes to a local, else a register
  // The constant pool in force where the function was defined. A pooled string
  // push resolves against this, and a function that runs later than its
  // defining block cannot rely on the pool the caller happens to hold.
  base::Vector<base::String> pool;
};

struct AsObject {
  base::UnorderedMap<base::String, AsValue> props;
  base::Vector<base::String> order;  // insertion order, for enumerate
  u32 prototype = 0;                 // 0 = none; object 0 is never a prototype
  bool is_function = false;
  bool is_array = false;
  bool is_movie_clip = false;
  AsFunctionBody body;
  NativeFn native = nullptr;
  // Set for a function that closed over a scope, and for a bound method.
  u32 scope = 0;
  // Host binding: which translated widget this clip or field stands for. The
  // Vm itself never interprets this; the host installs natives that do.
  u64 host = 0;
};

// A script the Vm can run: one action block, already disassembled.
struct AsScript {
  base::Vector<Action> actions;
  base::Vector<base::String> pool;
};

class Vm {
 public:
  Vm();

  // Loads an action block and returns its script index. The block is
  // disassembled once; functions defined inside it refer back to it.
  u32 AddScript(ByteSpan code);

  // Runs a whole script at top level with `this` bound to `self`.
  void Run(u32 script, const AsValue& self);

  // Calls a function value with `this` bound to `self`.
  AsValue Call(const AsValue& function, const AsValue& self,
               const base::Vector<AsValue>& args);

  // --- object model -------------------------------------------------------
  u32 NewObject(u32 prototype = 0);
  u32 NewArray();
  u32 NewNative(NativeFn fn);
  AsObject& Get(u32 index) { return objects_[index]; }
  const AsObject& Get(u32 index) const { return objects_[index]; }
  bool Valid(u32 index) const { return index != 0 && index < objects_.size(); }

  // Property access that walks the prototype chain.
  AsValue GetMember(const AsValue& target, base::StringRef name);
  void SetMember(const AsValue& target, base::StringRef name, const AsValue& value);

  // The game bridge. A menu reaches its host through
  // gfx.io.GameDelegate.call(), which the movies implement on top of
  // flash.external.ExternalInterface.call - so a host that answers this answers
  // the whole surface, without the Vm having to know what GameDelegate is.
  // `user` is handed back to the handler untouched.
  using ExternalHandler = AsValue (*)(void* user, Vm& vm, base::StringRef name,
                                      const base::Vector<AsValue>& args);
  void set_external_handler(ExternalHandler handler, void* user) {
    external_handler_ = handler;
    external_user_ = user;
  }
  AsValue DispatchExternal(base::StringRef name, const base::Vector<AsValue>& args);
  // Every external call the scripts made, in order, whether or not a handler
  // answered. This is the list of native functions a menu actually needs.
  const base::Vector<base::String>& external_calls() const { return external_calls_; }

  // Object.registerClass binds an export symbol to a class, which is how a
  // placed clip gets the behaviour its movie wrote for it.
  void RegisterClass(base::StringRef symbol, const AsValue& klass);
  AsValue RegisteredClass(base::StringRef symbol) const;
  // The prototype every movie clip inherits from, so a host can hang the clip
  // API on it once and have every clip answer to it.
  u32 movie_clip_prototype() const { return movie_clip_prototype_; }

  u32 global() const { return global_; }
  // The object every movie clip's script sees as `_root` / `_level0`.
  void set_root(const AsValue& root) { root_ = root; }
  const AsValue& root() const { return root_; }

  // --- conversions --------------------------------------------------------
  bool ToBool(const AsValue& v) const;
  f64 ToNumber(const AsValue& v) const;
  base::String ToString(const AsValue& v);

  // Whatever the script traced, in order. The menus trace a great deal, and it
  // is the most direct evidence that a script ran and took the branch expected.
  const base::Vector<base::String>& traces() const { return traces_; }
  // Instructions executed, so a runaway script can be told from a finished one.
  u64 steps() const { return steps_; }
  // Set once the step budget runs out; the run stops rather than hanging.
  bool exhausted() const { return exhausted_; }

 private:
  struct Frame;
  void Execute(u32 script, u32 first, u32 count, Frame& frame);
  AsValue CallInternal(const AsValue& function, const AsValue& self,
                       const base::Vector<AsValue>& args, u32 depth);
  u32 IndexOfOffset(const AsScript& script, u32 first, u32 count, u32 offset) const;
  AsValue ResolveVariable(Frame& frame, base::StringRef name);
  void AssignVariable(Frame& frame, base::StringRef name, const AsValue& value);
  void InstallStandardLibrary();
  AsValue MakeSuper(const AsValue& self);

  base::Vector<AsObject> objects_;
  base::Vector<AsScript> scripts_;
  base::Vector<base::String> traces_;
  u32 global_ = 0;
  u32 object_prototype_ = 0;
  u32 array_prototype_ = 0;
  u32 string_prototype_ = 0;
  u32 movie_clip_prototype_ = 0;
  u32 function_prototype_ = 0;
  ExternalHandler external_handler_ = nullptr;
  void* external_user_ = nullptr;
  base::Vector<base::String> external_calls_;
  base::UnorderedMap<base::String, AsValue> registered_classes_;
  AsValue root_;
  u64 steps_ = 0;
  bool exhausted_ = false;
  u32 depth_ = 0;
};

}  // namespace rx::swf

#endif  // RECREATION_SWF_VM_H_
