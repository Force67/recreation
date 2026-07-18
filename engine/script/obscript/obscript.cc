#include "script/obscript/obscript.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace rx::script::obscript {
namespace {

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

std::string_view Trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// The token type in one expression or statement line. Identifiers keep their
// authored case (editor ids are resolved case insensitively by the host).
struct Token {
  enum Kind { kNumber, kIdent, kOp, kString, kEnd } kind = kEnd;
  std::string text;
  f32 number = 0;
};

// Splits a line into tokens. Handles numbers, dotted identifiers, quoted
// strings, and the operator set (== != <= >= && || < > + - * / =).
base::Vector<Token> Tokenize(std::string_view line) {
  base::Vector<Token> out;
  size_t i = 0;
  while (i < line.size()) {
    char c = line[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '"') {
      size_t j = i + 1;
      while (j < line.size() && line[j] != '"') ++j;
      out.push_back({Token::kString, std::string(line.substr(i + 1, j - i - 1)), 0});
      i = j < line.size() ? j + 1 : j;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && i + 1 < line.size() && std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
      size_t j = i;
      while (j < line.size() &&
             (std::isdigit(static_cast<unsigned char>(line[j])) || line[j] == '.'))
        ++j;
      Token t{Token::kNumber, std::string(line.substr(i, j - i)), 0};
      t.number = static_cast<f32>(std::atof(t.text.c_str()));
      out.push_back(std::move(t));
      i = j;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t j = i;
      while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) ||
                                 line[j] == '_' || line[j] == '.'))
        ++j;
      out.push_back({Token::kIdent, std::string(line.substr(i, j - i)), 0});
      i = j;
      continue;
    }
    // Operators, longest match first.
    static const char* kTwo[] = {"==", "!=", "<=", ">=", "&&", "||"};
    bool matched = false;
    if (i + 1 < line.size()) {
      std::string two = std::string(line.substr(i, 2));
      for (const char* op : kTwo) {
        if (two == op) {
          out.push_back({Token::kOp, two, 0});
          i += 2;
          matched = true;
          break;
        }
      }
    }
    if (matched) continue;
    out.push_back({Token::kOp, std::string(1, c), 0});
    ++i;
  }
  out.push_back({Token::kEnd, "", 0});
  return out;
}

std::string FirstWord(std::string_view line) {
  std::string_view t = Trim(line);
  size_t sp = 0;
  while (sp < t.size() && !std::isspace(static_cast<unsigned char>(t[sp]))) ++sp;
  return std::string(t.substr(0, sp));
}

std::string AfterFirstWord(std::string_view line) {
  std::string_view t = Trim(line);
  size_t sp = 0;
  while (sp < t.size() && !std::isspace(static_cast<unsigned char>(t[sp]))) ++sp;
  return std::string(Trim(t.substr(sp)));
}

}  // namespace

bool Parse(std::string_view source, Script* out) {
  *out = Script{};
  // Split into cleaned lines: strip ';' comments (outside quotes) and trim.
  base::Vector<std::string> raw;
  size_t start = 0;
  auto flush = [&](size_t end) {
    std::string_view line = source.substr(start, end - start);
    bool in_string = false;
    size_t cut = line.size();
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"') in_string = !in_string;
      if (line[i] == ';' && !in_string) {
        cut = i;
        break;
      }
    }
    std::string_view trimmed = Trim(line.substr(0, cut));
    if (!trimmed.empty()) raw.push_back(std::string(trimmed));
  };
  for (size_t i = 0; i <= source.size(); ++i) {
    if (i == source.size() || source[i] == '\n' || source[i] == '\r') {
      flush(i);
      start = i + 1;
    }
  }

  bool have_name = false;
  Script::Block* current = nullptr;
  for (const std::string& line : raw) {
    std::string w = Lower(FirstWord(line));
    if (!have_name) {
      if (w == "scriptname" || w == "scn") {
        out->name = AfterFirstWord(line);
        have_name = true;
      }
      continue;
    }
    if (!current) {
      if (w == "begin") {
        std::string rest = AfterFirstWord(line);
        Script::Block block;
        block.type = Lower(FirstWord(rest));
        block.param = AfterFirstWord(rest);
        out->blocks.push_back(std::move(block));
        current = &out->blocks[out->blocks.size() - 1];
        continue;
      }
      Script::VarKind kind;
      if (w == "short" || w == "int" || w == "long") kind = Script::VarKind::kShort;
      else if (w == "float") kind = Script::VarKind::kFloat;
      else if (w == "ref" || w == "reference") kind = Script::VarKind::kRef;
      else continue;  // ScriptName duplicates, unknown directives: skip
      std::string var = FirstWord(AfterFirstWord(line));
      if (!var.empty()) out->vars.push_back({Lower(var), kind});
      continue;
    }
    if (w == "end") {
      current = nullptr;
      continue;
    }
    current->lines.push_back(line);
  }
  return have_name;
}

// ---- interpreter -----------------------------------------------------------

namespace {

// Recursive descent expression evaluator over a token stream. Precedence, low
// to high: || , && , == != , < > <= >= , + - , * / , unary, primary.
class Eval {
 public:
  Eval(const base::Vector<Token>& tokens, Instance* self, Host* host)
      : tokens_(tokens), self_(self), host_(host) {}

  f32 Parse() { return Or(); }
  bool AtEnd() const { return Peek().kind == Token::kEnd; }

 private:
  const Token& Peek() const { return tokens_[pos_]; }
  const Token& Next() { return tokens_[pos_ < tokens_.size() - 1 ? pos_++ : pos_]; }
  bool IsOp(const char* op) const { return Peek().kind == Token::kOp && Peek().text == op; }
  bool TakeOp(const char* op) {
    if (IsOp(op)) {
      ++pos_;
      return true;
    }
    return false;
  }

  f32 Or() {
    f32 v = And();
    while (TakeOp("||")) v = (v != 0 || And() != 0) ? 1.0f : 0.0f;
    return v;
  }
  f32 And() {
    f32 v = Compare();
    while (TakeOp("&&")) v = (v != 0 && Compare() != 0) ? 1.0f : 0.0f;
    return v;
  }
  f32 Compare() {
    f32 v = Relational();
    for (;;) {
      if (TakeOp("==") || TakeOp("=")) v = (v == Relational()) ? 1.0f : 0.0f;
      else if (TakeOp("!=")) v = (v != Relational()) ? 1.0f : 0.0f;
      else break;
    }
    return v;
  }
  f32 Relational() {
    f32 v = Additive();
    for (;;) {
      if (TakeOp("<=")) v = (v <= Additive()) ? 1.0f : 0.0f;
      else if (TakeOp(">=")) v = (v >= Additive()) ? 1.0f : 0.0f;
      else if (TakeOp("<")) v = (v < Additive()) ? 1.0f : 0.0f;
      else if (TakeOp(">")) v = (v > Additive()) ? 1.0f : 0.0f;
      else break;
    }
    return v;
  }
  f32 Additive() {
    f32 v = Multiplicative();
    for (;;) {
      if (TakeOp("+")) v += Multiplicative();
      else if (TakeOp("-")) v -= Multiplicative();
      else break;
    }
    return v;
  }
  f32 Multiplicative() {
    f32 v = Unary();
    for (;;) {
      if (TakeOp("*")) v *= Unary();
      else if (TakeOp("/")) {
        f32 d = Unary();
        v = d != 0 ? v / d : 0;
      } else break;
    }
    return v;
  }
  f32 Unary() {
    if (TakeOp("-")) return -Unary();
    if (TakeOp("!")) return Unary() == 0 ? 1.0f : 0.0f;
    return Primary();
  }
  f32 Primary();

  const base::Vector<Token>& tokens_;
  Instance* self_;
  Host* host_;
  size_t pos_ = 0;
};

}  // namespace

Instance::Instance(const Script* script, Host* host) : script_(script), host_(host) {
  for (const Script::Var& v : script_->vars) locals_[v.name] = 0.0f;
}

f32 Instance::GetVar(std::string_view name) const {
  auto it = locals_.find(Lower(name));
  return it != locals_.end() ? it->second : 0.0f;
}

void Instance::SetVar(std::string_view name, f32 value) { locals_[Lower(name)] = value; }

namespace {

// Reads a value referenced by an identifier: a local variable, a dotted remote
// reference (Quest.Var / Ref.GetStage), or a bare global.
f32 ResolveIdent(const std::string& ident, Instance* self, Host* host) {
  size_t dot = ident.find('.');
  if (dot == std::string::npos) {
    // A declared local wins; otherwise treat the bare name as a global.
    for (const Script::Var& v : self->script().vars)
      if (IEquals(v.name, ident)) return self->GetVar(ident);
    return host->GetGlobal(ident);
  }
  std::string owner = ident.substr(0, dot);
  std::string member = ident.substr(dot + 1);
  if (IEquals(member, "getstage")) return static_cast<f32>(host->GetStage(owner));
  return host->GetRemoteVar(owner, member);
}

f32 Eval::Primary() {
  const Token& t = Peek();
  if (t.kind == Token::kNumber) {
    Next();
    return t.number;
  }
  if (IsOp("(")) {
    Next();
    f32 v = Or();
    TakeOp(")");
    return v;
  }
  if (t.kind == Token::kIdent) {
    std::string ident = t.text;
    Next();
    // getstage <quest>: the one expression function we special case; its single
    // argument is a bare quest editor id, not a nested expression.
    if (IEquals(ident, "getstage") && Peek().kind == Token::kIdent) {
      std::string quest = Next().text;
      return static_cast<f32>(host_->GetStage(quest));
    }
    return ResolveIdent(ident, self_, host_);
  }
  Next();  // skip stray string/operator
  return 0;
}

// Assigns to a set target: a local, a dotted remote var, or a bare global.
void Assign(Instance* self, Host* host, const std::string& lval, f32 value) {
  size_t dot = lval.find('.');
  if (dot != std::string::npos) {
    host->SetRemoteVar(lval.substr(0, dot), lval.substr(dot + 1), value);
    return;
  }
  for (const Script::Var& v : self->script().vars) {
    if (IEquals(v.name, lval)) {
      self->SetVar(lval, value);
      return;
    }
  }
  host->SetGlobal(lval, value);
}

// Executes a non-control statement line (set / function call).
void ExecStatement(const std::string& line, Instance* self, Host* host) {
  base::Vector<Token> tokens = Tokenize(line);
  if (tokens.empty() || tokens[0].kind == Token::kEnd) return;
  if (tokens[0].kind == Token::kIdent && IEquals(tokens[0].text, "set")) {
    if (tokens.size() < 4 || tokens[1].kind != Token::kIdent) return;
    const std::string& lval = tokens[1].text;
    // tokens[2] is "to"; evaluate the rest.
    base::Vector<Token> rhs;
    for (size_t i = 3; i < tokens.size(); ++i) rhs.push_back(tokens[i]);
    Eval e(rhs, self, host);
    Assign(self, host, lval, e.Parse());
    return;
  }
  // Function-call statement: [Target.]Func arg...
  if (tokens[0].kind != Token::kIdent) return;
  std::string call = tokens[0].text;
  std::string target, fn = call;
  size_t dot = call.find('.');
  if (dot != std::string::npos) {
    target = call.substr(0, dot);
    fn = call.substr(dot + 1);
  }
  base::Vector<f32> args;
  base::Vector<std::string> text_args;
  for (size_t i = 1; i < tokens.size(); ++i) {
    const Token& t = tokens[i];
    if (t.kind == Token::kNumber) {
      args.push_back(t.number);
    } else if (t.kind == Token::kString) {
      text_args.push_back(t.text);
    } else if (t.kind == Token::kIdent) {
      // A declared local resolves to a value; anything else is an editor id
      // (a message, form, or reference the host resolves).
      bool is_local = false;
      for (const Script::Var& v : self->script().vars)
        if (IEquals(v.name, t.text)) is_local = true;
      if (is_local) args.push_back(self->GetVar(t.text));
      else text_args.push_back(t.text);
    }
  }
  if (IEquals(fn, "setstage") && !args.empty()) {
    host->SetStage(target, static_cast<i32>(args[0]));
    return;
  }
  host->Call(target, Lower(fn), args, text_args);
}

// An if-branch: the condition text ("" means else) and its body line range.
struct Segment {
  std::string cond;
  size_t body_start = 0;
  size_t body_end = 0;
};

// Scans an if block starting at `if_index`, collecting its branch segments;
// returns the index of the matching endif (or the line count if unterminated).
size_t ScanIf(const base::Vector<std::string>& lines, size_t if_index,
              base::Vector<Segment>* segs) {
  Segment cur;
  cur.cond = AfterFirstWord(lines[if_index]);
  cur.body_start = if_index + 1;
  int depth = 0;
  for (size_t i = if_index + 1; i < lines.size(); ++i) {
    std::string w = Lower(FirstWord(lines[i]));
    if (w == "if") {
      ++depth;
    } else if (w == "endif") {
      if (depth == 0) {
        cur.body_end = i;
        segs->push_back(std::move(cur));
        return i;
      }
      --depth;
    } else if (depth == 0 && w == "elseif") {
      cur.body_end = i;
      segs->push_back(std::move(cur));
      cur = Segment{};
      cur.cond = AfterFirstWord(lines[i]);
      cur.body_start = i + 1;
    } else if (depth == 0 && w == "else") {
      cur.body_end = i;
      segs->push_back(std::move(cur));
      cur = Segment{};
      cur.body_start = i + 1;  // empty cond = always
    }
  }
  cur.body_end = lines.size();
  segs->push_back(std::move(cur));
  return lines.size();
}

enum class Flow { kNormal, kReturn };

Flow ExecRange(const base::Vector<std::string>& lines, size_t lo, size_t hi, Instance* self,
               Host* host) {
  size_t i = lo;
  while (i < hi) {
    std::string w = Lower(FirstWord(lines[i]));
    if (w == "if") {
      base::Vector<Segment> segs;
      size_t endif = ScanIf(lines, i, &segs);
      for (const Segment& seg : segs) {
        bool take = seg.cond.empty();
        if (!take) {
          base::Vector<Token> tokens = Tokenize(seg.cond);
          Eval e(tokens, self, host);
          take = e.Parse() != 0;
        }
        if (take) {
          if (ExecRange(lines, seg.body_start, seg.body_end, self, host) == Flow::kReturn)
            return Flow::kReturn;
          break;
        }
      }
      i = endif + 1;
      continue;
    }
    if (w == "return") return Flow::kReturn;
    if (w == "endif" || w == "else" || w == "elseif") {  // stray, skip
      ++i;
      continue;
    }
    ExecStatement(lines[i], self, host);
    ++i;
  }
  return Flow::kNormal;
}

}  // namespace

bool Instance::Run(std::string_view event, std::string_view param) {
  for (const Script::Block& block : script_->blocks) {
    if (!IEquals(block.type, event)) continue;
    if (!param.empty() && !block.param.empty() && !IEquals(block.param, param)) continue;
    ExecRange(block.lines, 0, block.lines.size(), this, host_);
    return true;
  }
  return false;
}

}  // namespace rx::script::obscript
