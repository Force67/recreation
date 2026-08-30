#ifndef RECREATION_SWF_AVM2_H_
#define RECREATION_SWF_AVM2_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/swf/abc.h"
#include "core/types.h"

namespace rx::swf {

// An ActionScript 3 interpreter for the bytecode Fallout 4 and Starfield build
// their menus from.
//
// The AVM1 machine next door runs Skyrim's menus, and everything that made that
// worth doing applies here: a shipped AS3 screen is an empty frame until its
// own code fills it, and reading the code statically only gets as far as the
// literals it happens to leave lying about (see ParseListBindings, which is a
// peephole over exactly two of them).
//
// Scope: the instruction set the menus use, which is a smaller language than
// AVM2 as a whole. Of the 110 opcodes across Fallout 4's 217 screens, a few
// dozen carry nearly all the weight - property access, calls, the scope stack,
// branches and arithmetic - and the rest are either trivial or annotations
// (`debug`, `debugline`) the machine can step over.

class Avm2;

// A runtime value. Numbers are f64 and strings are copied, as in the language.
class As3Value {
 public:
  enum class Type : u8 { kUndefined, kNull, kBool, kNumber, kString, kObject };

  As3Value() = default;
  static As3Value Undefined() { return As3Value(); }
  static As3Value Null();
  static As3Value Bool(bool v);
  static As3Value Number(f64 v);
  static As3Value Str(base::StringRef v);
  static As3Value Obj(u32 index);

  Type type() const { return type_; }
  bool is_undefined() const { return type_ == Type::kUndefined; }
  bool is_null() const { return type_ == Type::kNull; }
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

using As3Native = As3Value (*)(Avm2& vm, const As3Value& self,
                               const base::Vector<As3Value>& args);

struct As3Object {
  base::UnorderedMap<base::String, As3Value> props;
  base::Vector<base::String> order;  // insertion order, for enumerating
  base::Vector<As3Value> slots;      // getslot / setslot, 1-based in the format
  u32 prototype = 0;                 // 0 = none
  // Set on the object that stands for a class: `constructprop` makes instances
  // from it and its traits are what an instance starts with.
  i32 klass = -1;    // index into AbcFile::classes, -1 when this is not a class
  u32 method = ~0u;  // a function object: the method it runs
  u32 unit = 0;      // which loaded abc the method or class belongs to
  As3Native native = nullptr;
  // Non-empty on a function the host answers, and on the object those come
  // from. `Shared.BGSExternalInterface.call(codeObj, name, ...)` looks the name
  // up on the code object the game handed the screen, so any name is a call.
  base::String external;
  bool is_code_object = false;
  bool is_function = false;
  bool is_class = false;
  bool is_array = false;
  // A stand-in for something the player would have put there: a display object
  // the timeline placed, or a child of one. Reading a member it does not have
  // yields another stand-in rather than undefined, which is what lets a panel
  // walk `MainPanel_mc.List_mc.entryList` before anything has built it.
  bool is_display = false;
};

class Avm2 {
 public:
  Avm2();

  // Loads a DoABC block's parsed contents. Classes become objects on the
  // global scope under their qualified names, so `getlex` finds them.
  void AddAbc(const AbcFile& abc);

  // Runs a class's instance constructor against a fresh object and returns it,
  // which is what a menu's own setup is: the class the movie binds to a symbol,
  // constructed. Undefined when the class has no body to run.
  As3Value Construct(base::StringRef class_name);

  // Calls a method body with `this` bound to `self`.
  As3Value Call(const As3Value& function, const As3Value& self,
                const base::Vector<As3Value>& args);

  // Calls a method on an instance by name, which is how the game talks to an
  // AS3 menu: a screen exposes `InitList`, `SetPlatform` and the rest, and the
  // host calls them the way it calls an AS2 movie's GameDelegate callbacks.
  // False when the instance has no such method.
  bool Invoke(const As3Value& instance, base::StringRef name,
              const base::Vector<As3Value>& args);

  // The object the game hands a screen to talk back through. Reading any name
  // off it yields a function that reaches the host, which is what
  // Shared.BGSExternalInterface.call does with it. This is AS3's spelling of
  // the AS2 GameDelegate, and a screen with no code object talks to nothing.
  u32 NewCodeObject();

  // `user` is handed back to the handler untouched.
  using ExternalHandler = As3Value (*)(void* user, Avm2& vm, base::StringRef name,
                                       const base::Vector<As3Value>& args);
  void set_external_handler(ExternalHandler handler, void* user) {
    external_handler_ = handler;
    external_user_ = user;
  }
  // Every call the screen made through its code object, in order, answered or
  // not. This is the list of host functions a menu actually needs.
  const base::Vector<base::String>& external_calls() const { return external_calls_; }

  // Runs the frame code the class `class_name` opens on against `instance`.
  // A movie's nested panels are timeline classes whose frame-1 script is what
  // says how they open, and which class a placement is comes from the movie's
  // SymbolClass rather than from the trait's declared type: the declaration is
  // only `flash.display.MovieClip`. So the host, which has the movie, is what
  // can pair a placement with its class.
  void RunOpeningFrame(base::StringRef class_name, const As3Value& instance);

  // --- object model -------------------------------------------------------
  u32 NewObject(u32 prototype = 0);
  u32 NewArray();
  u32 NewNative(As3Native fn);
  As3Object& Get(u32 index) { return objects_[index]; }
  const As3Object& Get(u32 index) const { return objects_[index]; }
  bool Valid(u32 index) const { return index != 0 && index < objects_.size(); }
  u32 object_count() const { return static_cast<u32>(objects_.size()); }

  As3Value GetProperty(const As3Value& target, base::StringRef name);
  void SetProperty(const As3Value& target, base::StringRef name, const As3Value& value);

  u32 global() const { return global_; }

  // --- conversions --------------------------------------------------------
  bool ToBool(const As3Value& v) const;
  f64 ToNumber(const As3Value& v) const;
  base::String ToString(const As3Value& v);

  // Whatever the scripts traced, and how far the machine got. A menu's own
  // `trace` is the most direct evidence that a branch was taken.
  const base::Vector<base::String>& traces() const { return traces_; }
  u64 steps() const { return steps_; }
  bool exhausted() const { return exhausted_; }
  // Opcodes the machine met and does not implement, by name and count. This is
  // the list of what to write next, rather than a guess at it.
  const base::UnorderedMap<base::String, u32>& unhandled() const { return unhandled_; }
  // Methods the code called that resolved to nothing, by name and count. A menu
  // calling `Math.floor` or `String.split` gets undefined and carries on
  // quietly, so this is what says the runtime is missing rather than the
  // instruction set.
  const base::UnorderedMap<base::String, u32>& unresolved() const { return unresolved_; }

 private:
  struct Frame;
  // The abc a method belongs to, since a movie can carry several DoABC blocks.
  struct Unit {
    const AbcFile* abc = nullptr;
  };

  As3Value Run(u32 unit, u32 method_index, const As3Value& self,
               const base::Vector<As3Value>& args, u32 depth);
  // Finds the object on the scope chain that carries `name`, or the global.
  As3Value FindProperty(Frame& frame, base::StringRef name, bool strict);
  u32 ClassObject(u32 unit, base::StringRef name);
  u32 NewDisplay(u32 prototype = 0);
  void InstallTraits(u32 unit, const AbcClass& definition, const As3Value& instance);
  // An object carrying a class's instance methods, and its base's behind it.
  // A display stand-in whose declared type names a class gets this as its
  // prototype, so the movie's own methods resolve on it.
  u32 MethodsOf(base::StringRef class_name, u32 depth);
  void RunFrameScripts(u32 unit, const AbcClass& definition, const As3Value& instance);
  void InstallClass(u32 unit, u32 class_index);

  base::Vector<As3Object> objects_;
  base::Vector<Unit> units_;
  base::Vector<base::String> traces_;
  base::UnorderedMap<base::String, u32> unhandled_;
  base::UnorderedMap<base::String, u32> unresolved_;
  // Qualified class name -> the object that stands for it.
  base::UnorderedMap<base::String, u32> classes_;
  base::UnorderedMap<base::String, u32> methods_;
  // (unit << 32 | method) -> the decoded body, so a method called in a loop is
  // decoded once. Held behind a pointer because a running method holds a
  // reference into this while it calls others, and the map rehashing under it
  // would leave that reference pointing at freed instructions.
  base::UnorderedMap<u64, base::UniquePointer<base::Vector<AbcInstruction>>> decoded_;
  ExternalHandler external_handler_ = nullptr;
  void* external_user_ = nullptr;
  base::Vector<base::String> external_calls_;
  u32 global_ = 0;
  u32 object_prototype_ = 0;
  u32 string_members_ = 0;
  u64 steps_ = 0;
  u32 depth_ = 0;
  bool exhausted_ = false;
};

}  // namespace rx::swf

#endif  // RECREATION_SWF_AVM2_H_
