#include "script/papyrus/value.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>

namespace rec::script::papyrus {
namespace {

const std::string kEmpty;

// libc++ (the NDK toolchain) does not implement std::from_chars for floating
// point and deletes the overload, so fall back to strtof there. Both do a
// locale-independent decimal parse, so the result matches the desktop path.
f32 ParseFloat(const std::string& s) {
#if defined(_LIBCPP_VERSION)
  return std::strtof(s.c_str(), nullptr);
#else
  f32 out = 0;
  std::from_chars(s.data(), s.data() + s.size(), out);
  return out;
#endif
}

// Papyrus float-to-string keeps trailing fractional digits trimmed, matching
// how the game prints floats in logs and HUD text.
std::string FloatToString(f32 v) {
  std::string s = std::format("{}", v);
  return s;
}

}  // namespace

const std::string& Value::as_string() const {
  if (type_ == ValueType::kString) return std::get<std::string>(data_);
  return kEmpty;
}

i32 Value::ToInt() const {
  switch (type_) {
    case ValueType::kInt:
      return std::get<i32>(data_);
    case ValueType::kFloat:
      return static_cast<i32>(std::get<f32>(data_));
    case ValueType::kBool:
      return std::get<bool>(data_) ? 1 : 0;
    case ValueType::kString: {
      const std::string& s = std::get<std::string>(data_);
      i32 out = 0;
      std::from_chars(s.data(), s.data() + s.size(), out);
      return out;
    }
    default:
      return 0;
  }
}

f32 Value::ToFloat() const {
  switch (type_) {
    case ValueType::kInt:
      return static_cast<f32>(std::get<i32>(data_));
    case ValueType::kFloat:
      return std::get<f32>(data_);
    case ValueType::kBool:
      return std::get<bool>(data_) ? 1.0f : 0.0f;
    case ValueType::kString:
      return ParseFloat(std::get<std::string>(data_));
    default:
      return 0;
  }
}

bool Value::ToBool() const {
  switch (type_) {
    case ValueType::kNone:
      return false;
    case ValueType::kInt:
      return std::get<i32>(data_) != 0;
    case ValueType::kFloat:
      return std::get<f32>(data_) != 0.0f;
    case ValueType::kBool:
      return std::get<bool>(data_);
    case ValueType::kString:
      return !std::get<std::string>(data_).empty();
    case ValueType::kObject:
      return std::get<ObjectRef>(data_).handle != 0;
    case ValueType::kArray:
      return std::get<ArrayRef>(data_).id != 0;
    case ValueType::kStruct:
      return std::get<StructRef>(data_).id != 0;
  }
  return false;
}

std::string Value::ToString() const {
  switch (type_) {
    case ValueType::kNone:
      return "None";
    case ValueType::kInt:
      return std::to_string(std::get<i32>(data_));
    case ValueType::kFloat:
      return FloatToString(std::get<f32>(data_));
    case ValueType::kBool:
      return std::get<bool>(data_) ? "True" : "False";
    case ValueType::kString:
      return std::get<std::string>(data_);
    case ValueType::kObject:
      return std::format("[object {:#x}]", std::get<ObjectRef>(data_).handle);
    case ValueType::kArray:
      return std::format("[array {}]", std::get<ArrayRef>(data_).id);
    case ValueType::kStruct:
      return std::format("[struct {}]", std::get<StructRef>(data_).id);
  }
  return "None";
}

bool Value::Equals(const Value& other) const {
  // None equals only None.
  if (type_ == ValueType::kNone || other.type_ == ValueType::kNone)
    return type_ == other.type_;

  // Strings compare textually only against strings.
  if (type_ == ValueType::kString || other.type_ == ValueType::kString) {
    if (type_ != other.type_) return false;
    return std::get<std::string>(data_) == std::get<std::string>(other.data_);
  }

  if (type_ == ValueType::kObject || other.type_ == ValueType::kObject)
    return type_ == other.type_ && as_object() == other.as_object();
  if (type_ == ValueType::kArray || other.type_ == ValueType::kArray)
    return type_ == other.type_ && as_array() == other.as_array();
  if (type_ == ValueType::kStruct || other.type_ == ValueType::kStruct)
    return type_ == other.type_ && as_struct() == other.as_struct();

  // Numeric/bool: promote to float when either side is float, else compare ints.
  if (type_ == ValueType::kFloat || other.type_ == ValueType::kFloat)
    return ToFloat() == other.ToFloat();
  return ToInt() == other.ToInt();
}

int Value::Compare(const Value& other) const {
  if (type_ == ValueType::kString && other.type_ == ValueType::kString) {
    int c = std::get<std::string>(data_).compare(std::get<std::string>(other.data_));
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  }
  if (type_ == ValueType::kFloat || other.type_ == ValueType::kFloat) {
    f32 a = ToFloat(), b = other.ToFloat();
    return a < b ? -1 : a > b ? 1 : 0;
  }
  i32 a = ToInt(), b = other.ToInt();
  return a < b ? -1 : a > b ? 1 : 0;
}

}  // namespace rec::script::papyrus
