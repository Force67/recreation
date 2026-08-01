#ifndef RECREATION_SCRIPT_PAPYRUS_INTERPRETER_H_
#define RECREATION_SCRIPT_PAPYRUS_INTERPRETER_H_

#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/script/papyrus/pex.h"
#include "components/script/papyrus/value.h"

namespace rx::script::papyrus {

// Everything the interpreter needs from the surrounding VM: cross-object calls,
// properties, member storage, state, type checks and arrays. The interpreter
// itself is pure (it only touches one function's registers); all effects that
// reach other instances, native code, or the engine route through here. The VM
// implements it; a test can implement a trivial version.
class VmInterface {
 public:
  virtual ~VmInterface() = default;

  virtual Value CallMethod(ObjectRef self, const base::String& method,
                           base::Vector<Value> args) = 0;
  virtual Value CallStatic(const base::String& script_type, const base::String& function,
                           base::Vector<Value> args) = 0;
  virtual Value CallParent(ObjectRef self, const base::String& method,
                           base::Vector<Value> args) = 0;

  virtual Value GetProperty(ObjectRef self, const base::String& property) = 0;
  virtual void SetProperty(ObjectRef self, const base::String& property, Value value) = 0;

  // Storage for a member variable on an instance, or null if the instance does
  // not carry it. The interpreter reads/writes through the returned slot.
  virtual Value* MemberVar(ObjectRef self, const base::String& name) = 0;

  virtual base::String CurrentState(ObjectRef self) = 0;
  virtual void GotoState(ObjectRef self, const base::String& state) = 0;

  // True if obj is None or an instance of type_name (or a subtype). Drives the
  // object cast opcode, which yields None on a failed downcast.
  virtual bool IsObjectOfType(ObjectRef obj, const base::String& type_name) = 0;

  virtual ArrayRef ArrayCreate(const base::String& element_type, i32 size) = 0;
  virtual i32 ArrayLength(ArrayRef array) = 0;
  virtual Value ArrayGet(ArrayRef array, i32 index) = 0;
  virtual void ArraySet(ArrayRef array, i32 index, Value value) = 0;
  virtual i32 ArrayFind(ArrayRef array, const Value& value, i32 start) = 0;
  virtual i32 ArrayRFind(ArrayRef array, const Value& value, i32 start) = 0;

  // Resizable-array and struct ops are Fallout 4/76 only; Skyrim arrays are
  // fixed-size and Skyrim has no structs. The Skyrim VM still implements them so
  // the one interpreter covers every dialect's opcodes.
  virtual void ArrayAdd(ArrayRef array, const Value& value, i32 count) = 0;
  virtual void ArrayInsert(ArrayRef array, i32 index, const Value& value) = 0;
  virtual void ArrayRemove(ArrayRef array, i32 index, i32 count) = 0;
  virtual void ArrayRemoveLast(ArrayRef array) = 0;
  virtual void ArrayClear(ArrayRef array) = 0;
  virtual StructRef StructCreate(const base::String& type_name) = 0;
  virtual Value StructGet(StructRef instance, const base::String& member) = 0;
  virtual void StructSet(StructRef instance, const base::String& member, Value value) = 0;
};

// Executes a single non-native Papyrus function to completion and returns its
// value (None if it falls off the end). Every Skyrim opcode (0x00-0x23) is
// implemented here. object is the script that defines fn, used for member
// variable types; self is the instance the function runs on.
Value ExecuteFunction(const PexFile& pex, const Object& object, const Function& fn, ObjectRef self,
                      base::Vector<Value> args, VmInterface& vm,
                      base::StringRef function_name = {});

}  // namespace rx::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_INTERPRETER_H_
