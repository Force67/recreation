#ifndef RECREATION_MASTERLIST_JSON_H_
#define RECREATION_MASTERLIST_JSON_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "core/types.h"

// Just enough JSON for one job: reading what the masterlist answers and
// writing what a host announces. Not a general library -- five fields do not
// justify a dependency, and the reader's contract is that anything it cannot
// make sense of is a refusal, never a guess.
//
// The document is a flat arena of nodes addressed by id, so nothing here needs
// an incomplete type inside a container. Node 0 is the root once Parse
// succeeds.

namespace rx::masterlist {

class JsonDoc {
 public:
  enum class Kind : u8 { kNull, kBool, kNumber, kString, kArray, kObject };

  static constexpr u32 kInvalid = 0xffffffffu;

  // False on malformed input, on nesting past kMaxDepth, or on a document
  // bigger than kMaxNodes. Trailing content after the root value is refused
  // too: a truncated or doubled body is not half a document.
  bool Parse(const base::String& text);

  bool valid() const { return !nodes_.empty(); }
  u32 root() const { return nodes_.empty() ? kInvalid : 0; }

  Kind KindOf(u32 node) const;
  u32 Count(u32 node) const;                // members or elements
  u32 Element(u32 node, u32 index) const;   // arrays
  u32 Member(u32 node, const char* key) const;  // objects; kInvalid when absent

  // Readers that answer with the fallback rather than failing, because every
  // field of a listing is optional from this side: a list that grows a field
  // must not break an older client, and one that drops a field must not crash
  // a newer one.
  base::String Str(u32 node, const char* fallback = "") const;
  f64 Num(u32 node, f64 fallback = 0) const;
  bool Bool(u32 node, bool fallback = false) const;

  // Sugar for the common "read this field off an object" shape.
  base::String MemberStr(u32 node, const char* key, const char* fallback = "") const;
  u64 MemberU64(u32 node, const char* key, u64 fallback = 0) const;
  u32 MemberU32(u32 node, const char* key, u32 fallback = 0) const;
  bool MemberBool(u32 node, const char* key, bool fallback = false) const;

 private:
  static constexpr u32 kMaxDepth = 32;
  static constexpr u32 kMaxNodes = 100000;

  struct Node {
    Kind kind = Kind::kNull;
    bool boolean = false;
    f64 number = 0;
    base::String text;
    base::Vector<u32> children;
    base::Vector<base::String> keys;  // objects only, parallel to children
  };

  bool ParseValue(const base::String& text, mem_size& i, u32 depth, u32* out);
  bool ParseString(const base::String& text, mem_size& i, base::String* out);
  bool ParseNumber(const base::String& text, mem_size& i, u32* out);
  u32 AddNode(Kind kind);

  base::Vector<Node> nodes_;
};

// Builds one flat JSON object. Keys are written in call order, which keeps a
// request body stable and diffable against a server log.
class JsonWriter {
 public:
  void Str(const char* key, const base::String& value);
  void Num(const char* key, u64 value);
  void Bool(const char* key, bool value);
  void StrArray(const char* key, const base::Vector<base::String>& values);

  base::String Finish();

 private:
  void Separator();

  base::String out_;
  bool any_ = false;
};

// Escapes one string as a JSON string literal, quotes included.
base::String JsonQuote(const base::String& value);

}  // namespace rx::masterlist

#endif  // RECREATION_MASTERLIST_JSON_H_
