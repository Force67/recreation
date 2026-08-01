#ifndef RECREATION_SCRIPT_PAPYRUS_VALUE_H_
#define RECREATION_SCRIPT_PAPYRUS_VALUE_H_

#include <string>
#include <variant>

#include "core/types.h"

namespace rx::script::papyrus {

// Papyrus is dynamically typed at runtime over six value kinds. Object and
// array values are reference handles: copying a Value copies the handle, the
// VM owns the pointed-to instance/storage. This matches Papyrus reference
// semantics (two variables holding the same object see each other's changes).
enum class ValueType : u8 {
  kNone,
  kInt,
  kFloat,
  kBool,
  kString,
  kObject,
  kArray,
  kStruct,  // Fallout 4/76 user-defined struct instance
};

// Handle to a script object instance (or engine-bound form). 0 means None. The
// VM's object table maps a handle to its concrete instance and script type.
struct ObjectRef {
  u64 handle = 0;
  bool operator==(const ObjectRef&) const = default;
};

// Handle to a VM-owned array. 0 means None.
struct ArrayRef {
  u32 id = 0;
  bool operator==(const ArrayRef&) const = default;
};

// Handle to a VM-owned struct instance (Fallout). 0 means None.
struct StructRef {
  u32 id = 0;
  bool operator==(const StructRef&) const = default;
};

// A dynamically typed Papyrus value. Construct via the static factories;
// read via type() and the as_*/To* accessors.
class Value {
 public:
  Value() = default;  // None

  static Value Int(i32 v) { return Value(ValueType::kInt, v); }
  static Value Float(f32 v) { return Value(ValueType::kFloat, v); }
  static Value Bool(bool v) { return Value(ValueType::kBool, v); }
  static Value Str(std::string v) { return Value(ValueType::kString, std::move(v)); }
  static Value Object(ObjectRef v) { return Value(ValueType::kObject, v); }
  static Value Array(ArrayRef v) { return Value(ValueType::kArray, v); }
  static Value Struct(StructRef v) { return Value(ValueType::kStruct, v); }

  ValueType type() const { return type_; }
  bool is_none() const { return type_ == ValueType::kNone; }

  // Raw accessors. Each assumes the value is of that type; mismatched access
  // returns a zero/default. Use the To* helpers when coercion is wanted.
  i32 as_int() const { return type_ == ValueType::kInt ? std::get<i32>(data_) : 0; }
  f32 as_float() const { return type_ == ValueType::kFloat ? std::get<f32>(data_) : 0; }
  bool as_bool() const { return type_ == ValueType::kBool ? std::get<bool>(data_) : false; }
  const std::string& as_string() const;
  ObjectRef as_object() const {
    return type_ == ValueType::kObject ? std::get<ObjectRef>(data_) : ObjectRef{};
  }
  ArrayRef as_array() const {
    return type_ == ValueType::kArray ? std::get<ArrayRef>(data_) : ArrayRef{};
  }
  StructRef as_struct() const {
    return type_ == ValueType::kStruct ? std::get<StructRef>(data_) : StructRef{};
  }

  // Papyrus coercion rules, used by the cast opcode and arithmetic promotion.
  i32 ToInt() const;
  f32 ToFloat() const;
  bool ToBool() const;
  std::string ToString() const;

  // Equality (cmp_eq) with numeric promotion. cmp_lt/le/gt/ge use Compare,
  // which returns -1/0/1 and is defined for numeric and string operands.
  bool Equals(const Value& other) const;
  int Compare(const Value& other) const;

 private:
  using Storage =
      std::variant<std::monostate, i32, f32, bool, std::string, ObjectRef, ArrayRef, StructRef>;
  template <typename T>
  Value(ValueType t, T v) : type_(t), data_(std::move(v)) {}

  ValueType type_ = ValueType::kNone;
  Storage data_;
};

}  // namespace rx::script::papyrus

#endif  // RECREATION_SCRIPT_PAPYRUS_VALUE_H_
