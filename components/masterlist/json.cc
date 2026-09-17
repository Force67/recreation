#include "components/masterlist/json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rx::masterlist {
namespace {

void SkipSpace(const base::String& text, mem_size& i) {
  while (i < text.size()) {
    const char c = text[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      return;
    ++i;
  }
}

bool Literal(const base::String& text, mem_size& i, const char* word) {
  const mem_size len = std::strlen(word);
  if (i + len > text.size())
    return false;
  for (mem_size k = 0; k < len; ++k)
    if (text[i + k] != word[k])
      return false;
  i += len;
  return true;
}

bool ParseHex4(const base::String& text, mem_size& i, u32* out) {
  if (i + 4 > text.size())
    return false;
  u32 value = 0;
  for (int k = 0; k < 4; ++k) {
    const char c = text[i + static_cast<mem_size>(k)];
    u32 digit;
    if (c >= '0' && c <= '9')
      digit = static_cast<u32>(c - '0');
    else if (c >= 'a' && c <= 'f')
      digit = static_cast<u32>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      digit = static_cast<u32>(c - 'A' + 10);
    else
      return false;
    value = value * 16 + digit;
  }
  i += 4;
  *out = value;
  return true;
}

void AppendUtf8(base::String* out, u32 cp) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

}  // namespace

u32 JsonDoc::AddNode(Kind kind) {
  Node node;
  node.kind = kind;
  nodes_.push_back(std::move(node));
  return static_cast<u32>(nodes_.size() - 1);
}

bool JsonDoc::Parse(const base::String& text) {
  nodes_.clear();
  mem_size i = 0;
  u32 root = kInvalid;
  if (!ParseValue(text, i, 0, &root)) {
    nodes_.clear();
    return false;
  }
  SkipSpace(text, i);
  // Content after the root value means the body is not the document it claims
  // to be (a doubled response, a truncated proxy write).
  if (i != text.size()) {
    nodes_.clear();
    return false;
  }
  return root == 0;
}

bool JsonDoc::ParseValue(const base::String& text, mem_size& i, u32 depth, u32* out) {
  if (depth > kMaxDepth || nodes_.size() >= kMaxNodes)
    return false;
  SkipSpace(text, i);
  if (i >= text.size())
    return false;

  const char c = text[i];
  if (c == '{') {
    const u32 node = AddNode(Kind::kObject);
    ++i;
    SkipSpace(text, i);
    if (i < text.size() && text[i] == '}') {
      ++i;
      *out = node;
      return true;
    }
    for (;;) {
      SkipSpace(text, i);
      base::String key;
      if (!ParseString(text, i, &key))
        return false;
      SkipSpace(text, i);
      if (i >= text.size() || text[i] != ':')
        return false;
      ++i;
      u32 child = kInvalid;
      if (!ParseValue(text, i, depth + 1, &child))
        return false;
      // Re-index rather than holding a reference: the recursion above may have
      // grown the arena and moved it.
      nodes_[node].keys.push_back(key);
      nodes_[node].children.push_back(child);
      SkipSpace(text, i);
      if (i < text.size() && text[i] == ',') {
        ++i;
        continue;
      }
      if (i < text.size() && text[i] == '}') {
        ++i;
        break;
      }
      return false;
    }
    *out = node;
    return true;
  }

  if (c == '[') {
    const u32 node = AddNode(Kind::kArray);
    ++i;
    SkipSpace(text, i);
    if (i < text.size() && text[i] == ']') {
      ++i;
      *out = node;
      return true;
    }
    for (;;) {
      u32 child = kInvalid;
      if (!ParseValue(text, i, depth + 1, &child))
        return false;
      nodes_[node].children.push_back(child);
      SkipSpace(text, i);
      if (i < text.size() && text[i] == ',') {
        ++i;
        continue;
      }
      if (i < text.size() && text[i] == ']') {
        ++i;
        break;
      }
      return false;
    }
    *out = node;
    return true;
  }

  if (c == '"') {
    base::String value;
    if (!ParseString(text, i, &value))
      return false;
    const u32 node = AddNode(Kind::kString);
    nodes_[node].text = value;
    *out = node;
    return true;
  }

  if (c == 't' || c == 'f') {
    const bool value = c == 't';
    if (!Literal(text, i, value ? "true" : "false"))
      return false;
    const u32 node = AddNode(Kind::kBool);
    nodes_[node].boolean = value;
    *out = node;
    return true;
  }

  if (c == 'n') {
    if (!Literal(text, i, "null"))
      return false;
    *out = AddNode(Kind::kNull);
    return true;
  }

  return ParseNumber(text, i, out);
}

bool JsonDoc::ParseString(const base::String& text, mem_size& i, base::String* out) {
  if (i >= text.size() || text[i] != '"')
    return false;
  ++i;
  out->clear();
  while (i < text.size()) {
    const char c = text[i++];
    if (c == '"')
      return true;
    if (c != '\\') {
      // A raw control byte inside a string is malformed JSON, and letting one
      // through would put it in a menu label.
      if (static_cast<unsigned char>(c) < 0x20)
        return false;
      out->push_back(c);
      continue;
    }
    if (i >= text.size())
      return false;
    const char escape = text[i++];
    switch (escape) {
      case '"': out->push_back('"'); break;
      case '\\': out->push_back('\\'); break;
      case '/': out->push_back('/'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case 'u': {
        u32 cp = 0;
        if (!ParseHex4(text, i, &cp))
          return false;
        if (cp >= 0xd800 && cp <= 0xdbff) {
          // A high surrogate is only half a character; the low half has to
          // follow or the string is broken.
          if (i + 1 >= text.size() || text[i] != '\\' || text[i + 1] != 'u')
            return false;
          i += 2;
          u32 low = 0;
          if (!ParseHex4(text, i, &low) || low < 0xdc00 || low > 0xdfff)
            return false;
          cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
          return false;  // a lone low surrogate
        }
        AppendUtf8(out, cp);
        break;
      }
      default:
        return false;
    }
  }
  return false;  // unterminated
}

bool JsonDoc::ParseNumber(const base::String& text, mem_size& i, u32* out) {
  const mem_size begin = i;
  while (i < text.size()) {
    const char c = text[i];
    const bool numeric = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
                         c == 'e' || c == 'E';
    if (!numeric)
      break;
    ++i;
  }
  if (i == begin || i - begin > 63)
    return false;

  char buffer[64] = {};
  for (mem_size k = begin; k < i; ++k)
    buffer[k - begin] = text[k];
  char* end = nullptr;
  const double value = std::strtod(buffer, &end);
  if (end == buffer || *end != '\0')
    return false;

  const u32 node = AddNode(Kind::kNumber);
  nodes_[node].number = value;
  *out = node;
  return true;
}

JsonDoc::Kind JsonDoc::KindOf(u32 node) const {
  return node < nodes_.size() ? nodes_[node].kind : Kind::kNull;
}

u32 JsonDoc::Count(u32 node) const {
  return node < nodes_.size() ? static_cast<u32>(nodes_[node].children.size()) : 0;
}

u32 JsonDoc::Element(u32 node, u32 index) const {
  if (node >= nodes_.size() || index >= nodes_[node].children.size())
    return kInvalid;
  return nodes_[node].children[index];
}

u32 JsonDoc::Member(u32 node, const char* key) const {
  if (node >= nodes_.size() || nodes_[node].kind != Kind::kObject)
    return kInvalid;
  const Node& object = nodes_[node];
  for (mem_size i = 0; i < object.keys.size(); ++i)
    if (object.keys[i] == key)
      return object.children[i];
  return kInvalid;
}

base::String JsonDoc::Str(u32 node, const char* fallback) const {
  if (node >= nodes_.size() || nodes_[node].kind != Kind::kString)
    return base::String(fallback);
  return nodes_[node].text;
}

f64 JsonDoc::Num(u32 node, f64 fallback) const {
  if (node >= nodes_.size() || nodes_[node].kind != Kind::kNumber)
    return fallback;
  return nodes_[node].number;
}

bool JsonDoc::Bool(u32 node, bool fallback) const {
  if (node >= nodes_.size() || nodes_[node].kind != Kind::kBool)
    return fallback;
  return nodes_[node].boolean;
}

base::String JsonDoc::MemberStr(u32 node, const char* key, const char* fallback) const {
  return Str(Member(node, key), fallback);
}

u64 JsonDoc::MemberU64(u32 node, const char* key, u64 fallback) const {
  const f64 value = Num(Member(node, key), -1.0);
  if (value < 0)
    return fallback;
  return static_cast<u64>(value);
}

u32 JsonDoc::MemberU32(u32 node, const char* key, u32 fallback) const {
  const u64 value = MemberU64(node, key, fallback);
  return value > 0xffffffffull ? fallback : static_cast<u32>(value);
}

bool JsonDoc::MemberBool(u32 node, const char* key, bool fallback) const {
  return Bool(Member(node, key), fallback);
}

base::String JsonQuote(const base::String& value) {
  base::String out;
  out.push_back('"');
  for (mem_size i = 0; i < value.size(); ++i) {
    const char c = value[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escape[8] = {};
          std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(c));
          out += escape;
        } else {
          // Anything else, UTF-8 included, is already a legal JSON string byte.
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

void JsonWriter::Separator() {
  if (any_)
    out_ += ",";
  any_ = true;
}

void JsonWriter::Str(const char* key, const base::String& value) {
  Separator();
  out_ += JsonQuote(base::String(key));
  out_ += ":";
  out_ += JsonQuote(value);
}

void JsonWriter::Num(const char* key, u64 value) {
  Separator();
  out_ += JsonQuote(base::String(key));
  out_ += ":";
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  out_ += buffer;
}

void JsonWriter::Bool(const char* key, bool value) {
  Separator();
  out_ += JsonQuote(base::String(key));
  out_ += ":";
  out_ += value ? "true" : "false";
}

void JsonWriter::StrArray(const char* key, const base::Vector<base::String>& values) {
  Separator();
  out_ += JsonQuote(base::String(key));
  out_ += ":[";
  for (mem_size i = 0; i < values.size(); ++i) {
    if (i != 0)
      out_ += ",";
    out_ += JsonQuote(values[i]);
  }
  out_ += "]";
}

base::String JsonWriter::Finish() {
  base::String out("{");
  out += out_;
  out += "}";
  return out;
}

}  // namespace rx::masterlist
