#include "components/swf/decompile.h"

#include <base/containers/unordered_map.h>
#include <base/memory/move.h>
#include <base/strings/format.h>

#include <cmath>

#include "components/swf/avm1.h"

namespace rx::swf {
namespace {

// Binding strength, so the printer only parenthesises where it has to.
enum Prec : int {
  kComma = 0,
  kAssign = 1,
  kTernary = 2,
  kLogicalOr = 3,
  kLogicalAnd = 4,
  kBitOr = 5,
  kBitXor = 6,
  kBitAnd = 7,
  kEquality = 8,
  kRelational = 9,
  kShift = 10,
  kAdditive = 11,
  kMultiplicative = 12,
  kUnary = 13,
  kPostfix = 14,
  kPrimary = 15,
};

struct Expr {
  base::String text;
  int precedence = kPrimary;
  bool side_effects = false;  // a call or assignment: dropping it changes meaning
};

Expr Primary(base::String text) {
  return Expr{base::move(text), kPrimary, false};
}

base::String Wrap(const Expr& e, int required) {
  if (e.precedence >= required)
    return e.text;
  base::String out = "(";
  out += e.text;
  out += ')';
  return out;
}

// AVM1 numbers are doubles; integers print without a fractional part so the
// output reads like the source rather than like a float dump.
base::String FormatNumber(f64 value) {
  if (value == static_cast<f64>(static_cast<i64>(value)) && std::fabs(value) < 1e15)
    return base::Format("{}", static_cast<i64>(value));
  return base::Format("{}", value);
}

base::String QuoteString(base::StringRef text) {
  base::String out = "\"";
  for (mem_size i = 0; i < text.size(); ++i) {
    const char c = text[i];
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<u8>(c) < 0x20)
          out += base::Format("\\x{:02x}", static_cast<u32>(static_cast<u8>(c)));
        else
          out += c;
    }
  }
  out += '"';
  return out;
}

bool IsIdentifier(base::StringRef text) {
  if (text.empty())
    return false;
  const char first = text[0];
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
        first == '_' || first == '$'))
    return false;
  for (mem_size i = 1; i < text.size(); ++i) {
    const char c = text[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '$'))
      return false;
  }
  return true;
}

// The name of a MovieClip property, indexed the way GetProperty/SetProperty do.
base::StringRef PropertyName(int index) {
  static const char* const kNames[] = {
      "_x",          "_y",         "_xscale",     "_yscale",    "_currentframe",
      "_totalframes", "_alpha",    "_visible",    "_width",     "_height",
      "_rotation",   "_target",    "_framesloaded", "_name",    "_droptarget",
      "_url",        "_highquality", "_focusrect", "_soundbuftime", "_quality",
      "_xmouse",     "_ymouse"};
  if (index < 0 || index >= static_cast<int>(sizeof(kNames) / sizeof(kNames[0])))
    return "";
  return kNames[index];
}

struct Statement {
  base::String text;
  u32 indent = 0;
};

// One decompiled scope: a script, a function body, or a with() block. Holds the
// operand stack, the register file and the emitted statements.
// What a register currently holds. `statement` points at the `var _locN = ...`
// line the binding emitted, so an assignment that immediately follows can take
// the temporary's place and the name never appears in the output.
struct RegisterBinding {
  base::String text;      // what reading the register substitutes
  u32 statement = ~0u;    // the emitted definition, when it is a temporary
  base::String expression;
};

class Scope {
 public:
  Scope(ByteSpan code,
        u32 depth,
        const base::Vector<base::String>& inherited_pool,
        const Action* function)
      : code_(code), depth_(depth), pool_(inherited_pool) {
    SeedRegisters(function);
  }

  base::String Run();

 private:
  void Structure(mem_size from, mem_size to);
  void EmitLinear(mem_size from, mem_size to);
  void Step(const Action& action);
  void Emit(base::String text);
  void FlushPending();

  Expr Pop();
  void Push(Expr e);
  Expr ValueToExpr(const Value& v);
  Expr Register(u32 index);
  base::Vector<Expr> PopArguments();
  base::String Member(const Expr& object, const Expr& member);
  void Binary(const char* symbol, int precedence);
  void CallLike(bool is_new, bool method);
  base::String DecompileBody(ByteSpan body, const Action* function);
  void SeedRegisters(const Action* function);
  // `var _loc1 = <fn>; X = _loc1;` is how the compiler builds a class: fold the
  // pair back into `X = <fn>;` and point the register at X.
  bool CollapseTemp(base::StringRef target, const Expr& value);
  // Recognises `a && b` / `a || b`, which the compiler lowers to a branch over
  // the right operand. Returns the index to continue from, or `from` when the
  // actions at `branch` are not one of those shapes.
  mem_size ShortCircuit(mem_size branch, mem_size to);
  // A `switch` lowers to a run of conditional branches into the case bodies,
  // ending in a jump to the default. Rebuilds it as the equivalent
  // if / else-if / else chain; returns the index to continue from, or `branch`
  // when the shape does not match.
  mem_size ConditionalChain(mem_size branch, mem_size to);

  mem_size IndexOfOffset(u32 offset) const;

  ByteSpan code_;
  u32 depth_ = 0;
  base::Vector<Action> actions_;
  base::Vector<base::String> pool_;
  base::Vector<Expr> stack_;
  base::UnorderedMap<u32, RegisterBinding> registers_;
  base::Vector<Statement> statements_;
  u32 indent_ = 0;
  u32 temp_counter_ = 0;
  // The action the expression currently on the stack started at. A loop is
  // recognised by its tail jumping back to where its condition begins, which
  // is exactly the last point the stack was empty.
  mem_size expression_start_ = 0;
};

base::String Scope::DecompileBody(ByteSpan body, const Action* function) {
  if (depth_ > 24)
    return "/* nesting limit reached */";
  Scope nested(body, depth_ + 1, pool_, function);
  return nested.Run();
}

// DefineFunction2 preloads the implicit values into the low registers before
// the body runs, in the fixed order the flags are listed. Without this the body
// reads as r1/r2 instead of `this` and `super`.
void Scope::SeedRegisters(const Action* function) {
  if (!function || function->code != op::kDefineFunction2)
    return;
  u32 next = 1;
  const u16 flags = function->function_flags;
  if (flags & fn_flags::kPreloadThis)
    registers_[next++] = RegisterBinding{"this", ~0u, base::String()};
  if (flags & fn_flags::kPreloadArguments)
    registers_[next++] = RegisterBinding{"arguments", ~0u, base::String()};
  if (flags & fn_flags::kPreloadSuper)
    registers_[next++] = RegisterBinding{"super", ~0u, base::String()};
  if (flags & fn_flags::kPreloadRoot)
    registers_[next++] = RegisterBinding{"_root", ~0u, base::String()};
  if (flags & fn_flags::kPreloadParent)
    registers_[next++] = RegisterBinding{"_parent", ~0u, base::String()};
  if (flags & fn_flags::kPreloadGlobal)
    registers_[next++] = RegisterBinding{"_global", ~0u, base::String()};
  for (mem_size i = 0; i < function->param_registers.size() &&
                       i < function->strings.size();
       ++i) {
    const u8 slot = function->param_registers[i];
    if (slot != 0)
      registers_[slot] = RegisterBinding{function->strings[i], ~0u, base::String()};
  }
}

bool Scope::CollapseTemp(base::StringRef target, const Expr& value) {
  for (auto entry : registers_) {
    RegisterBinding& binding = entry.value;
    if (binding.statement == ~0u || binding.text != value.text)
      continue;
    if (binding.statement >= statements_.size())
      continue;
    base::String line(target);
    line += " = ";
    line += binding.expression;
    line += ';';
    statements_[binding.statement].text = base::move(line);
    binding.text = base::String(target);
    binding.statement = ~0u;
    binding.expression = base::String();
    return true;
  }
  return false;
}

void Scope::Emit(base::String text) {
  statements_.push_back(Statement{base::move(text), indent_});
}

// Anything still on the stack at a statement boundary was computed for its side
// effects (a call whose result is ignored) or is a compiler temporary. Calls are
// kept as statements; pure expressions are dropped with a note.
void Scope::FlushPending() {
  for (mem_size i = 0; i < stack_.size(); ++i) {
    if (stack_[i].side_effects) {
      base::String line = stack_[i].text;
      line += ';';
      Emit(base::move(line));
    }
  }
  stack_.clear();
}

Expr Scope::Pop() {
  if (stack_.empty())
    return Expr{"undefined", kPrimary, false};
  Expr top = base::move(stack_[stack_.size() - 1]);
  stack_.pop_back();
  return top;
}

void Scope::Push(Expr e) {
  stack_.push_back(base::move(e));
}

Expr Scope::Register(u32 index) {
  const RegisterBinding* stored = registers_.find(index);
  if (stored)
    return Expr{stored->text, kPrimary, false};
  return Primary(base::Format("r{}", index));
}

Expr Scope::ValueToExpr(const Value& v) {
  switch (v.kind) {
    case Value::Kind::kString:
      return Expr{QuoteString(v.text), kPrimary, false};
    case Value::Kind::kFloat:
    case Value::Kind::kDouble:
    case Value::Kind::kInt:
      return Expr{FormatNumber(v.number), v.number < 0 ? kUnary : kPrimary, false};
    case Value::Kind::kNull:
      return Primary("null");
    case Value::Kind::kUndefined:
      return Primary("undefined");
    case Value::Kind::kRegister:
      return Register(v.index);
    case Value::Kind::kBool:
      return Primary(v.boolean ? "true" : "false");
    case Value::Kind::kConstant:
      if (v.index < pool_.size())
        return Expr{QuoteString(pool_[v.index]), kPrimary, false};
      return Primary(base::Format("const{}", v.index));
  }
  return Primary("undefined");
}

// Calls take their argument count from the stack, then the arguments in reverse.
base::Vector<Expr> Scope::PopArguments() {
  const Expr count = Pop();
  i64 n = 0;
  for (mem_size i = 0; i < count.text.size(); ++i) {
    const char c = count.text[i];
    if (c >= '0' && c <= '9')
      n = n * 10 + (c - '0');
    else if (i == 0 && c == '-')
      n = -1;
    else {
      n = -1;
      break;
    }
  }
  base::Vector<Expr> args;
  if (n < 0 || n > 256)
    return args;  // a computed argument count: give up rather than unbalance
  for (i64 i = 0; i < n; ++i)
    args.push_back(Pop());
  return args;
}

base::String Scope::Member(const Expr& object, const Expr& member) {
  // A constant string member prints as `.name` when it is a legal identifier.
  if (member.precedence == kPrimary && member.text.size() >= 2 &&
      member.text[0] == '"' && member.text[member.text.size() - 1] == '"') {
    base::String inner;
    for (mem_size i = 1; i + 1 < member.text.size(); ++i)
      inner.push_back(member.text[i]);
    if (IsIdentifier(inner)) {
      base::String out = Wrap(object, kPostfix);
      out += '.';
      out += inner;
      return out;
    }
  }
  base::String out = Wrap(object, kPostfix);
  out += '[';
  out += member.text;
  out += ']';
  return out;
}

void Scope::Binary(const char* symbol, int precedence) {
  const Expr rhs = Pop();
  const Expr lhs = Pop();
  base::String out = Wrap(lhs, precedence);
  out += ' ';
  out += symbol;
  out += ' ';
  // Right operand needs the next tighter level so `a - (b - c)` keeps its
  // parentheses while `a - b - c` does not gain any.
  out += Wrap(rhs, precedence + 1);
  Push(Expr{base::move(out), precedence, lhs.side_effects || rhs.side_effects});
}

void Scope::CallLike(bool is_new, bool method) {
  Expr callee;
  if (method) {
    const Expr name = Pop();
    const Expr object = Pop();
    const bool anonymous = name.text == "undefined" || name.text == "\"\"" ||
                           name.text == "null";
    callee = Primary(anonymous ? object.text : Member(object, name));
  } else {
    const Expr name = Pop();
    // CallFunction takes the name as a value; a literal becomes an identifier.
    if (name.text.size() >= 2 && name.text[0] == '"' &&
        name.text[name.text.size() - 1] == '"') {
      base::String inner;
      for (mem_size i = 1; i + 1 < name.text.size(); ++i)
        inner.push_back(name.text[i]);
      callee = Primary(IsIdentifier(inner) ? inner : base::Format("eval({})", name.text));
    } else {
      callee = name;
    }
  }

  const base::Vector<Expr> args = PopArguments();
  base::String out;
  if (is_new)
    out += "new ";
  out += Wrap(callee, kPostfix);
  out += '(';
  for (mem_size i = 0; i < args.size(); ++i) {
    if (i)
      out += ", ";
    out += args[i].text;
  }
  out += ')';
  Push(Expr{base::move(out), kPostfix, true});
}

void Scope::Step(const Action& action) {
  switch (action.code) {
    case op::kConstantPool:
      pool_ = action.strings;
      break;

    case op::kPush:
      for (const Value& v : action.values)
        Push(ValueToExpr(v));
      break;

    case op::kPop: {
      const Expr top = Pop();
      if (top.side_effects) {
        base::String line = top.text;
        line += ';';
        Emit(base::move(line));
      }
      break;
    }

    case op::kPushDuplicate:
      if (!stack_.empty())
        Push(stack_[stack_.size() - 1]);
      break;

    case op::kStackSwap:
      if (stack_.size() >= 2) {
        Expr a = base::move(stack_[stack_.size() - 1]);
        stack_[stack_.size() - 1] = base::move(stack_[stack_.size() - 2]);
        stack_[stack_.size() - 2] = base::move(a);
      }
      break;

    case op::kStoreRegister: {
      // Peeks rather than pops. A value with side effects is bound to a named
      // temporary so the call happens once; anything else is remembered
      // symbolically and substituted wherever the register is read.
      if (stack_.empty())
        break;
      Expr& top = stack_[stack_.size() - 1];
      // A value that is expensive to repeat (a call) or long (a function
      // literal) is bound once; anything short is substituted symbolically.
      bool multiline = false;
      for (mem_size i = 0; i < top.text.size() && !multiline; ++i)
        multiline = top.text[i] == '\n';
      if (top.side_effects || multiline) {
        base::String name = base::Format("_loc{}", ++temp_counter_);
        base::String line = "var ";
        line += name;
        line += " = ";
        line += top.text;
        line += ';';
        RegisterBinding binding;
        binding.text = name;
        binding.expression = top.text;
        binding.statement = static_cast<u32>(statements_.size());
        Emit(base::move(line));
        registers_[action.byte_arg] = base::move(binding);
        top = Expr{base::move(name), kPrimary, false};
      } else {
        registers_[action.byte_arg] = RegisterBinding{top.text, ~0u, base::String()};
      }
      break;
    }

    case op::kGetVariable: {
      const Expr name = Pop();
      if (name.text.size() >= 2 && name.text[0] == '"' &&
          name.text[name.text.size() - 1] == '"') {
        base::String inner;
        for (mem_size i = 1; i + 1 < name.text.size(); ++i)
          inner.push_back(name.text[i]);
        // Dotted paths ("_root.foo.bar") are legal variable names in AVM1 and
        // read better unquoted.
        bool path = !inner.empty();
        for (mem_size i = 0; i < inner.size() && path; ++i) {
          const char c = inner[i];
          path = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.' || c == '/';
        }
        Push(Primary(path ? inner : base::Format("eval({})", name.text)));
      } else {
        Push(Expr{base::Format("eval({})", name.text), kPostfix, false});
      }
      break;
    }

    case op::kSetVariable: {
      const Expr value = Pop();
      const Expr name = Pop();
      base::String target = name.text;
      if (target.size() >= 2 && target[0] == '"' && target[target.size() - 1] == '"') {
        base::String inner;
        for (mem_size i = 1; i + 1 < target.size(); ++i)
          inner.push_back(target[i]);
        target = inner;
      } else {
        target = base::Format("set({}, ", name.text);
        target += value.text;
        target += ");";
        Emit(base::move(target));
        break;
      }
      if (CollapseTemp(target, value))
        break;
      base::String line = target;
      line += " = ";
      line += value.text;
      line += ';';
      Emit(base::move(line));
      break;
    }

    case op::kDefineLocal: {
      const Expr value = Pop();
      const Expr name = Pop();
      base::String line = "var ";
      if (name.text.size() >= 2 && name.text[0] == '"')
        for (mem_size i = 1; i + 1 < name.text.size(); ++i)
          line.push_back(name.text[i]);
      else
        line += name.text;
      line += " = ";
      line += value.text;
      line += ';';
      Emit(base::move(line));
      break;
    }

    case op::kDefineLocal2: {
      const Expr name = Pop();
      base::String line = "var ";
      if (name.text.size() >= 2 && name.text[0] == '"')
        for (mem_size i = 1; i + 1 < name.text.size(); ++i)
          line.push_back(name.text[i]);
      else
        line += name.text;
      line += ';';
      Emit(base::move(line));
      break;
    }

    case op::kGetMember: {
      const Expr member = Pop();
      const Expr object = Pop();
      Push(Expr{Member(object, member), kPostfix, object.side_effects});
      break;
    }

    case op::kSetMember: {
      const Expr value = Pop();
      const Expr member = Pop();
      const Expr object = Pop();
      base::String line = Member(object, member);
      if (CollapseTemp(line, value))
        break;
      line += " = ";
      line += value.text;
      line += ';';
      Emit(base::move(line));
      break;
    }

    case op::kCallFunction:
      CallLike(false, false);
      break;
    case op::kCallMethod:
      CallLike(false, true);
      break;
    case op::kNewObject:
      CallLike(true, false);
      break;
    case op::kNewMethod:
      CallLike(true, true);
      break;

    case op::kInitArray: {
      const base::Vector<Expr> items = PopArguments();
      base::String out = "[";
      for (mem_size i = 0; i < items.size(); ++i) {
        if (i)
          out += ", ";
        out += items[i].text;
      }
      out += ']';
      Push(Expr{base::move(out), kPrimary, false});
      break;
    }

    case op::kInitObject: {
      const Expr count = Pop();
      i64 n = 0;
      bool numeric = !count.text.empty();
      for (mem_size i = 0; i < count.text.size() && numeric; ++i) {
        const char c = count.text[i];
        if (c >= '0' && c <= '9')
          n = n * 10 + (c - '0');
        else
          numeric = false;
      }
      if (!numeric || n > 256) {
        Push(Primary("{}"));
        break;
      }
      base::Vector<base::String> pairs;
      for (i64 i = 0; i < n; ++i) {
        const Expr value = Pop();
        const Expr name = Pop();
        base::String pair;
        if (name.text.size() >= 2 && name.text[0] == '"') {
          base::String inner;
          for (mem_size k = 1; k + 1 < name.text.size(); ++k)
            inner.push_back(name.text[k]);
          pair = IsIdentifier(inner) ? inner : name.text;
        } else {
          pair = name.text;
        }
        pair += ": ";
        pair += value.text;
        pairs.push_back(base::move(pair));
      }
      base::String out = "{";
      for (mem_size i = pairs.size(); i > 0; --i) {
        if (i != pairs.size())
          out += ", ";
        out += pairs[i - 1];
      }
      out += '}';
      Push(Expr{base::move(out), kPrimary, false});
      break;
    }

    case op::kAdd:
    case op::kAdd2:
      Binary("+", kAdditive);
      break;
    case op::kSubtract:
      Binary("-", kAdditive);
      break;
    case op::kMultiply:
      Binary("*", kMultiplicative);
      break;
    case op::kDivide:
      Binary("/", kMultiplicative);
      break;
    case op::kModulo:
      Binary("%", kMultiplicative);
      break;
    case op::kEquals:
    case op::kEquals2:
      Binary("==", kEquality);
      break;
    case op::kStrictEquals:
      Binary("===", kEquality);
      break;
    case op::kLess:
    case op::kLess2:
      Binary("<", kRelational);
      break;
    case op::kGreater:
      Binary(">", kRelational);
      break;
    case op::kAnd:
      Binary("&&", kLogicalAnd);
      break;
    case op::kOr:
      Binary("||", kLogicalOr);
      break;
    case op::kBitAnd:
      Binary("&", kBitAnd);
      break;
    case op::kBitOr:
      Binary("|", kBitOr);
      break;
    case op::kBitXor:
      Binary("^", kBitXor);
      break;
    case op::kBitLShift:
      Binary("<<", kShift);
      break;
    case op::kBitRShift:
      Binary(">>", kShift);
      break;
    case op::kBitURShift:
      Binary(">>>", kShift);
      break;
    case op::kInstanceOf:
      Binary("instanceof", kRelational);
      break;
    case op::kStringEquals:
      Binary("eq", kEquality);
      break;
    case op::kStringLess:
      Binary("lt", kRelational);
      break;
    case op::kStringGreater:
      Binary("gt", kRelational);
      break;
    case op::kStringAdd:
      Binary("add", kAdditive);
      break;

    case op::kNot: {
      const Expr value = Pop();
      base::String out = "!";
      out += Wrap(value, kUnary);
      Push(Expr{base::move(out), kUnary, value.side_effects});
      break;
    }

    case op::kIncrement:
    case op::kDecrement: {
      const Expr value = Pop();
      base::String out = Wrap(value, kAdditive);
      out += action.code == op::kIncrement ? " + 1" : " - 1";
      Push(Expr{base::move(out), kAdditive, value.side_effects});
      break;
    }

    case op::kTypeOf: {
      const Expr value = Pop();
      base::String out = "typeof ";
      out += Wrap(value, kUnary);
      Push(Expr{base::move(out), kUnary, false});
      break;
    }

    case op::kToNumber:
    case op::kToInteger:
    case op::kToString:
    case op::kTargetPath:
    case op::kRandomNumber:
    case op::kStringLength:
    case op::kMbStringLength:
    case op::kCharToAscii:
    case op::kMbCharToAscii:
    case op::kAsciiToChar:
    case op::kMbAsciiToChar: {
      const char* name = "Number";
      switch (action.code) {
        case op::kToInteger:
          name = "int";
          break;
        case op::kToString:
          name = "String";
          break;
        case op::kTargetPath:
          name = "targetPath";
          break;
        case op::kRandomNumber:
          name = "random";
          break;
        case op::kStringLength:
        case op::kMbStringLength:
          name = "length";
          break;
        case op::kCharToAscii:
        case op::kMbCharToAscii:
          name = "ord";
          break;
        case op::kAsciiToChar:
        case op::kMbAsciiToChar:
          name = "chr";
          break;
        default:
          break;
      }
      const Expr value = Pop();
      Push(Expr{base::Format("{}({})", name, value.text), kPostfix, false});
      break;
    }

    case op::kStringExtract:
    case op::kMbStringExtract: {
      const Expr count = Pop();
      const Expr index = Pop();
      const Expr text = Pop();
      Push(Expr{base::Format("substring({}, {}, {})", text.text, index.text, count.text),
                kPostfix, false});
      break;
    }

    case op::kGetTime:
      Push(Expr{"getTimer()", kPostfix, false});
      break;

    case op::kCastOp: {
      const Expr object = Pop();
      const Expr type = Pop();
      Push(Expr{base::Format("{}({})", type.text, object.text), kPostfix, false});
      break;
    }

    case op::kExtends: {
      const Expr super = Pop();
      const Expr sub = Pop();
      Emit(base::Format("{} extends {};", sub.text, super.text));
      break;
    }

    case op::kImplementsOp: {
      const base::Vector<Expr> interfaces = PopArguments();
      const Expr sub = Pop();
      base::String line = sub.text;
      line += " implements ";
      for (mem_size i = 0; i < interfaces.size(); ++i) {
        if (i)
          line += ", ";
        line += interfaces[i].text;
      }
      line += ';';
      Emit(base::move(line));
      break;
    }

    case op::kDelete: {
      const Expr member = Pop();
      const Expr object = Pop();
      Emit(base::Format("delete {};", Member(object, member)));
      break;
    }

    case op::kDelete2: {
      const Expr name = Pop();
      Emit(base::Format("delete {};", name.text));
      break;
    }

    case op::kTrace:
      Emit(base::Format("trace({});", Pop().text));
      break;

    case op::kThrow:
      Emit(base::Format("throw {};", Pop().text));
      break;

    case op::kReturn:
      Emit(base::Format("return {};", Pop().text));
      break;

    case op::kGetProperty: {
      const Expr index = Pop();
      const Expr target = Pop();
      i64 n = -1;
      if (!index.text.empty() && index.text[0] >= '0' && index.text[0] <= '9') {
        n = 0;
        for (mem_size i = 0; i < index.text.size(); ++i)
          n = n * 10 + (index.text[i] - '0');
      }
      const base::StringRef property = PropertyName(static_cast<int>(n));
      if (property.empty())
        Push(Expr{base::Format("getProperty({}, {})", target.text, index.text), kPostfix,
                  false});
      else
        Push(Expr{base::Format("{}.{}", target.text, property), kPostfix, false});
      break;
    }

    case op::kSetProperty: {
      const Expr value = Pop();
      const Expr index = Pop();
      const Expr target = Pop();
      i64 n = -1;
      if (!index.text.empty() && index.text[0] >= '0' && index.text[0] <= '9') {
        n = 0;
        for (mem_size i = 0; i < index.text.size(); ++i)
          n = n * 10 + (index.text[i] - '0');
      }
      const base::StringRef property = PropertyName(static_cast<int>(n));
      if (property.empty())
        Emit(base::Format("setProperty({}, {}, {});", target.text, index.text,
                          value.text));
      else
        Emit(base::Format("{}.{} = {};", target.text, property, value.text));
      break;
    }

    case op::kPlay:
      Emit("play();");
      break;
    case op::kStop:
      Emit("stop();");
      break;
    case op::kNextFrame:
      Emit("nextFrame();");
      break;
    case op::kPrevFrame:
      Emit("prevFrame();");
      break;
    case op::kStopSounds:
      Emit("stopAllSounds();");
      break;
    case op::kToggleQuality:
      Emit("toggleHighQuality();");
      break;
    case op::kEndDrag:
      Emit("stopDrag();");
      break;

    case op::kStartDrag: {
      const Expr target = Pop();
      const Expr lock = Pop();
      Emit(base::Format("startDrag({}, {});", target.text, lock.text));
      break;
    }

    case op::kCloneSprite: {
      const Expr depth = Pop();
      const Expr name = Pop();
      const Expr source = Pop();
      Emit(base::Format("duplicateMovieClip({}, {}, {});", source.text, name.text,
                        depth.text));
      break;
    }

    case op::kRemoveSprite:
      Emit(base::Format("removeMovieClip({});", Pop().text));
      break;

    case op::kGotoFrame:
      Emit(base::Format("gotoAndStop({});", action.word_arg + 1));
      break;

    case op::kGotoLabel:
      Emit(base::Format("gotoAndStop(\"{}\");", action.name));
      break;

    case op::kGotoFrame2: {
      const Expr frame = Pop();
      Emit(base::Format("{}({});", (action.byte_arg & 1) ? "gotoAndPlay" : "gotoAndStop",
                        frame.text));
      break;
    }

    case op::kCall:
      Emit(base::Format("call({});", Pop().text));
      break;

    case op::kGetUrl:
      Emit(base::Format("getURL(\"{}\", \"{}\");", action.name, action.secondary));
      break;

    case op::kGetUrl2: {
      const Expr target = Pop();
      const Expr url = Pop();
      Emit(base::Format("getURL({}, {});", url.text, target.text));
      break;
    }

    case op::kSetTarget:
      Emit(base::Format("setTarget(\"{}\");", action.name));
      break;

    case op::kSetTarget2:
      Emit(base::Format("setTarget({});", Pop().text));
      break;

    case op::kEnumerate:
    case op::kEnumerate2: {
      // for..in pushes the property names then a null sentinel; the loop that
      // consumes them is left as a plain while over the sentinel.
      const Expr target = Pop();
      Emit(base::Format("// for (var k in {}) -- enumerated below", target.text));
      Push(Primary("null"));
      break;
    }

    case op::kDefineFunction:
    case op::kDefineFunction2: {
      base::String header = "function ";
      header += action.name;
      header += '(';
      for (mem_size i = 0; i < action.strings.size(); ++i) {
        if (i)
          header += ", ";
        header += action.strings[i];
      }
      header += ") {";

      const mem_size body_start = action.end;
      const mem_size body_end = body_start + action.body_size;
      base::String body;
      if (body_end <= code_.size())
        body = DecompileBody(code_.subspan(body_start, action.body_size), &action);

      if (action.name.empty()) {
        // Anonymous: the function object is a value, so it has to stay inline.
        base::String out = header;
        base::String line;
        for (mem_size i = 0; i <= body.size(); ++i) {
          if (i == body.size() || body[i] == '\n') {
            if (!line.empty()) {
              out += "\n  ";
              out += line;
            }
            line = base::String();
          } else {
            line.push_back(body[i]);
          }
        }
        out += "\n}";
        Push(Expr{base::move(out), kPrimary, false});
      } else {
        Emit(base::move(header));
        ++indent_;
        // The nested scope already indented relative to itself; re-emit its
        // lines at this scope's depth so the whole file lines up.
        base::String line;
        for (mem_size i = 0; i <= body.size(); ++i) {
          if (i == body.size() || body[i] == '\n') {
            if (!line.empty())
              Emit(base::move(line));
            line = base::String();
          } else {
            line.push_back(body[i]);
          }
        }
        --indent_;
        Emit("}");
      }
      break;
    }

    case op::kWith: {
      const Expr target = Pop();
      base::String header = "with (";
      header += target.text;
      header += ") {";
      Emit(base::move(header));
      ++indent_;
      const mem_size body_start = action.end;
      if (body_start + action.body_size <= code_.size()) {
        base::String body =
            DecompileBody(code_.subspan(body_start, action.body_size), nullptr);
        base::String line;
        for (mem_size i = 0; i <= body.size(); ++i) {
          if (i == body.size() || body[i] == '\n') {
            if (!line.empty())
              Emit(base::move(line));
            line = base::String();
          } else {
            line.push_back(body[i]);
          }
        }
      }
      --indent_;
      Emit("}");
      break;
    }

    case op::kEnd:
    case op::kJump:
    case op::kIf:
      break;  // handled by Structure

    default:
      Emit(base::Format("// unhandled action 0x{:02x} ({})", action.code,
                        OpName(action.code)));
      break;
  }
}

mem_size Scope::IndexOfOffset(u32 offset) const {
  for (mem_size i = 0; i < actions_.size(); ++i)
    if (actions_[i].offset == offset)
      return i;
  return actions_.size();
}

void Scope::EmitLinear(mem_size from, mem_size to) {
  for (mem_size i = from; i < to; ++i) {
    const Action& action = actions_[i];
    // gotoAndPlay/gotoAndStop compile to a goto action followed by Play or
    // Stop. Fold the pair back so it reads as the one call it was written as.
    const bool is_goto = action.code == op::kGotoFrame ||
                         action.code == op::kGotoLabel ||
                         action.code == op::kGotoFrame2;
    if (is_goto && i + 1 < to) {
      const u8 next = actions_[i + 1].code;
      if (next == op::kPlay || next == op::kStop) {
        const char* call = next == op::kPlay ? "gotoAndPlay" : "gotoAndStop";
        if (action.code == op::kGotoLabel)
          Emit(base::Format("{}(\"{}\");", call, action.name));
        else if (action.code == op::kGotoFrame)
          Emit(base::Format("{}({});", call, action.word_arg + 1));
        else
          Emit(base::Format("{}({});", call, Pop().text));
        ++i;
        continue;
      }
    }

    Step(action);
    // A statement boundary: nothing an action left behind is needed by the next
    // one, so anything with side effects becomes its own statement.
    const bool boundary = action.code == op::kSetMember ||
                          action.code == op::kSetVariable ||
                          action.code == op::kDefineLocal ||
                          action.code == op::kDefineLocal2 ||
                          action.code == op::kTrace || action.code == op::kReturn;
    if (boundary && !stack_.empty())
      FlushPending();
    if (stack_.empty())
      expression_start_ = i + 1;
  }
}

// `a && b` and `a || b` compile to a branch that skips the right operand, not
// to a statement. Recognising them here keeps them expressions instead of
// turning every one into an empty if.
mem_size Scope::ShortCircuit(mem_size branch, mem_size to) {
  if (branch == 0 || branch + 1 >= to)
    return branch;
  if (actions_[branch + 1].code != op::kPop)
    return branch;

  bool logical_and = false;
  if (actions_[branch - 1].code == op::kPushDuplicate) {
    logical_and = false;
  } else if (branch >= 2 && actions_[branch - 1].code == op::kNot &&
             actions_[branch - 2].code == op::kPushDuplicate) {
    logical_and = true;
  } else {
    return branch;
  }

  const u32 join_offset =
      static_cast<u32>(static_cast<i32>(actions_[branch].end) + actions_[branch].jump);
  const mem_size join = IndexOfOffset(join_offset);
  if (join <= branch + 1 || join > to)
    return branch;
  if (stack_.size() < 2)
    return branch;

  Pop();  // the duplicate the branch tested
  for (mem_size i = branch + 2; i < join; ++i)
    Step(actions_[i]);
  if (stack_.size() < 2)
    return branch;

  const Expr rhs = Pop();
  const Expr lhs = Pop();
  const int precedence = logical_and ? kLogicalAnd : kLogicalOr;
  base::String out = Wrap(lhs, precedence);
  out += logical_and ? " && " : " || ";
  out += Wrap(rhs, precedence + 1);
  Push(Expr{base::move(out), precedence, lhs.side_effects || rhs.side_effects});
  return join;
}

mem_size Scope::ConditionalChain(mem_size branch, mem_size to) {
  // Pass one is read-only: prove the shape before touching the stack.
  base::Vector<mem_size> tests;    // the If actions
  base::Vector<mem_size> targets;  // the case body each one enters
  mem_size chain_end = to;
  mem_size fallthrough = to;

  mem_size k = branch;
  while (true) {
    if (k >= to || actions_[k].code != op::kIf)
      return branch;
    const u32 offset =
        static_cast<u32>(static_cast<i32>(actions_[k].end) + actions_[k].jump);
    const mem_size target = IndexOfOffset(offset);
    if (target <= k || target > to)
      return branch;
    tests.push_back(k);
    targets.push_back(target);

    mem_size next = k + 1;
    while (next < to && actions_[next].code != op::kIf && actions_[next].code != op::kJump)
      ++next;
    if (next >= to)
      return branch;
    if (actions_[next].code == op::kIf) {
      k = next;
      continue;
    }
    const u32 default_offset =
        static_cast<u32>(static_cast<i32>(actions_[next].end) + actions_[next].jump);
    chain_end = next;
    fallthrough = IndexOfOffset(default_offset);
    break;
  }

  if (tests.size() < 2 || fallthrough <= chain_end || fallthrough > to)
    return branch;
  for (mem_size j = 0; j < targets.size(); ++j) {
    if (targets[j] <= chain_end || targets[j] > to)
      return branch;
    if (j > 0 && targets[j] <= targets[j - 1])
      return branch;
  }
  if (targets[targets.size() - 1] > fallthrough)
    return branch;

  // Every case body ends by jumping past the default; that target is where the
  // whole construct rejoins.
  mem_size end = fallthrough;
  for (mem_size j = 0; j < targets.size(); ++j) {
    const mem_size body_end = j + 1 < targets.size() ? targets[j + 1] : fallthrough;
    if (body_end == 0 || body_end - 1 < targets[j])
      continue;
    const Action& last = actions_[body_end - 1];
    if (last.code != op::kJump)
      continue;
    const mem_size join =
        IndexOfOffset(static_cast<u32>(static_cast<i32>(last.end) + last.jump));
    if (join > end && join <= to)
      end = join;
  }

  // Pass two: the conditions are pure, so collecting them emits nothing.
  base::Vector<base::String> conditions;
  conditions.push_back(Wrap(Pop(), kComma));
  for (mem_size j = 1; j < tests.size(); ++j) {
    EmitLinear(tests[j - 1] + 1, tests[j]);
    conditions.push_back(Wrap(Pop(), kComma));
  }
  EmitLinear(tests[tests.size() - 1] + 1, chain_end);

  for (mem_size j = 0; j < targets.size(); ++j) {
    mem_size body_end = j + 1 < targets.size() ? targets[j + 1] : fallthrough;
    if (body_end > targets[j] && actions_[body_end - 1].code == op::kJump)
      --body_end;
    Emit(j == 0 ? base::Format("if ({}) {{", conditions[j])
                : base::Format("}} else if ({}) {{", conditions[j]));
    ++indent_;
    Structure(targets[j], body_end);
    --indent_;
  }
  if (fallthrough < end) {
    Emit("} else {");
    ++indent_;
    Structure(fallthrough, end);
    --indent_;
  }
  Emit("}");
  return end;
}

// Recovers the control-flow shapes the ActionScript 2 compiler emits. Anything
// outside them falls back to a label and an explicit goto.
void Scope::Structure(mem_size from, mem_size to) {
  // A do/while is a conditional branch back to an earlier action; the head has
  // to be known before the walk reaches it so the opening brace lands there.
  base::Vector<mem_size> do_heads;
  for (mem_size k = from; k < to; ++k) {
    if (actions_[k].code != op::kIf)
      continue;
    const u32 back = static_cast<u32>(static_cast<i32>(actions_[k].end) + actions_[k].jump);
    const mem_size head = IndexOfOffset(back);
    if (head < k && head >= from)
      do_heads.push_back(head);
  }

  mem_size i = from;
  mem_size linear_start = i;
  expression_start_ = i;
  base::Vector<mem_size> open_loops;

  while (i < to) {
    bool is_head = false;
    for (mem_size k = 0; k < do_heads.size(); ++k)
      is_head = is_head || do_heads[k] == i;
    bool already_open = false;
    for (mem_size k = 0; k < open_loops.size(); ++k)
      already_open = already_open || open_loops[k] == i;
    if (is_head && !already_open) {
      EmitLinear(linear_start, i);
      Emit("do {");
      ++indent_;
      open_loops.push_back(i);
      linear_start = i;
      expression_start_ = i;
    }

    const Action& action = actions_[i];
    if (action.code != op::kIf && action.code != op::kJump) {
      ++i;
      continue;
    }

    EmitLinear(linear_start, i);

    if (action.code == op::kIf) {
      const mem_size folded = ShortCircuit(i, to);
      if (folded != i) {
        i = folded;
        linear_start = i;
        continue;
      }
      const mem_size chained = ConditionalChain(i, to);
      if (chained != i) {
        i = chained;
        linear_start = i;
        expression_start_ = i;
        continue;
      }
    }

    const u32 target_offset = static_cast<u32>(static_cast<i32>(action.end) + action.jump);
    const mem_size target = IndexOfOffset(target_offset);

    if (action.code == op::kJump) {
      if (target > i && target <= to) {
        // A forward jump with nothing between here and there is the tail of an
        // if/else the caller already consumed; anything else is unstructured.
        if (target != i + 1)
          Emit(base::Format("// jump -> {:04x}", target_offset));
        i = target;
        linear_start = i;
        expression_start_ = i;
        continue;
      }
      Emit(base::Format("// backward jump -> {:04x} (loop tail)", target_offset));
      ++i;
      linear_start = i;
      expression_start_ = i;
      continue;
    }

    // Conditional. The branch is taken when the top of stack is true, so the
    // fall-through runs under the negated condition.
    const Expr condition = Pop();
    base::String test;
    if (condition.precedence == kUnary && !condition.text.empty() &&
        condition.text[0] == '!') {
      for (mem_size k = 1; k < condition.text.size(); ++k)
        test.push_back(condition.text[k]);
      // Strip the parentheses Wrap() added around the negated operand.
      if (test.size() >= 2 && test[0] == '(' && test[test.size() - 1] == ')') {
        base::String inner;
        for (mem_size k = 1; k + 1 < test.size(); ++k)
          inner.push_back(test[k]);
        test = base::move(inner);
      }
    } else {
      test = "!";
      test += Wrap(condition, kUnary);
    }

    if (target <= i || target > to) {
      // The tail of a do/while, when it branches back to the head this scope
      // opened; anything else is a shape the structurer does not know.
      if (!open_loops.empty() && open_loops[open_loops.size() - 1] == target) {
        base::String keep = Wrap(condition, kComma);
        --indent_;
        Emit(base::Format("}} while ({});", keep));
        open_loops.pop_back();
      } else {
        Emit(base::Format("// conditional branch -> {:04x} when !({})", target_offset,
                          test));
      }
      ++i;
      linear_start = i;
      expression_start_ = i;
      continue;
    }

    const mem_size body_end = target;
    const Action& last = actions_[body_end - 1];
    const bool ends_with_jump = last.code == op::kJump;
    const u32 last_target =
        ends_with_jump ? static_cast<u32>(static_cast<i32>(last.end) + last.jump) : 0;

    // while: the block the branch skips ends with a jump back to the condition.
    const mem_size condition_start = expression_start_ <= i ? expression_start_ : i;
    if (ends_with_jump && last_target <= actions_[condition_start].offset &&
        IndexOfOffset(last_target) <= condition_start) {
      Emit(base::Format("while ({}) {{", test));
      ++indent_;
      Structure(i + 1, body_end - 1);
      --indent_;
      Emit("}");
      i = body_end;
      linear_start = i;
      expression_start_ = i;
      continue;
    }

    if (ends_with_jump && last_target > target_offset) {
      const mem_size else_end = IndexOfOffset(last_target);
      if (else_end <= to) {
        Emit(base::Format("if ({}) {{", test));
        ++indent_;
        Structure(i + 1, body_end - 1);
        --indent_;
        Emit("} else {");
        ++indent_;
        Structure(body_end, else_end);
        --indent_;
        Emit("}");
        i = else_end;
        linear_start = i;
        expression_start_ = i;
        continue;
      }
    }

    Emit(base::Format("if ({}) {{", test));
    ++indent_;
    Structure(i + 1, body_end);
    --indent_;
    Emit("}");
    i = body_end;
    linear_start = i;
    expression_start_ = i;
  }

  if (linear_start < to)
    EmitLinear(linear_start, to);
  FlushPending();
}

base::String Scope::Run() {
  actions_ = Disassemble(code_);

  // Function and with() bodies live in the bytes that follow their action, and
  // the linear disassembly walked straight into them. Drop those, the nested
  // scopes decompile them.
  base::Vector<Action> top_level;
  mem_size skip_until = 0;
  for (mem_size i = 0; i < actions_.size(); ++i) {
    const Action& action = actions_[i];
    if (action.offset < skip_until)
      continue;
    top_level.push_back(action);
    if (action.code == op::kDefineFunction || action.code == op::kDefineFunction2 ||
        action.code == op::kWith)
      skip_until = action.end + action.body_size;
  }
  actions_ = base::move(top_level);

  Structure(0, actions_.size());

  base::String out;
  for (const Statement& statement : statements_) {
    bool start_of_line = true;
    for (mem_size i = 0; i < statement.text.size(); ++i) {
      if (start_of_line) {
        for (u32 k = 0; k < statement.indent; ++k)
          out += "  ";
        start_of_line = false;
      }
      out += statement.text[i];
      if (statement.text[i] == '\n')
        start_of_line = true;
    }
    out += '\n';
  }
  return out;
}

}  // namespace

base::String Decompile(ByteSpan code) {
  base::Vector<base::String> pool;
  Scope scope(code, 0, pool, nullptr);
  return scope.Run();
}

base::String Disassembly(ByteSpan code) {
  base::Vector<base::String> pool;
  base::String out;
  for (const Action& action : Disassemble(code)) {
    if (action.code == op::kConstantPool)
      pool = action.strings;
    out += FormatAction(action, pool);
    out += '\n';
  }
  return out;
}

base::Vector<base::String> ConstantStrings(ByteSpan code) {
  base::Vector<base::String> out;
  for (const Action& action : Disassemble(code)) {
    if (action.code != op::kConstantPool)
      continue;
    for (const base::String& s : action.strings)
      out.push_back(s);
  }
  return out;
}

}  // namespace rx::swf
