#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "script/papyrus/transpile_internal.h"

namespace rec::script::papyrus::detail {
namespace {

// Reconstructs one Papyrus function body as C# statements. It inlines the
// compiler's temporaries back into expressions, rebuilds property, array, and
// call syntax, and structures the control flow, falling back to a labelled-goto
// rendering when the flow graph is not reducible.
class Decompiler {
 public:
  explicit Decompiler(const DecompileCtx& ctx)
      : pex_(ctx.pex),
        backing_(ctx.backing),
        fn_rename_(ctx.fn_rename),
        case_names_(ctx.case_names),
        case_fns_(ctx.case_fns),
        opts_(ctx.opts) {}

  void Emit(const Function& fn, std::string& out, int indent, const std::string& value_alias) {
    fn_ = &fn;
    value_alias_ = value_alias;
    temp_expr_.clear();
    type_of_.clear();
    read_count_.clear();
    indent_ = indent;
    mat_counter_ = 0;
    loops_.clear();
    materialized_.clear();
    mat_name_.clear();

    BuildSymbolTables();
    CountReads();

    // Render into a scratch buffer, structured if the flow graph allows it and a
    // goto rendering otherwise. The pass collects the hoisted-temp set, declared
    // at the top below: such a temp is written and read across sibling blocks, so
    // a block-local `var` would not be in scope at every use.
    std::string scratch;
    out_ = &scratch;
    bool ok = EmitStructured(0, static_cast<int>(fn.code.size()));
    if (!ok) {
      scratch.clear();
      temp_expr_.clear();
      materialized_.clear();
      mat_counter_ = 0;
      Line("// note: control flow was not reducible; rendered with goto/labels.");
      EmitGoto();
    }

    out_ = &out;
    for (const auto& [local, papyrus_type] : materialized_) {
      std::string ty = DeclTypeFor(papyrus_type, harness());
      if (ty == "void") ty = harness() ? "dynamic" : "object";  // a temp always holds a value
      Line(ty + " " + local + " = default;");
    }
    out += scratch;
  }

 private:
  const std::string& Name(const VariableData& v) const { return pex_.Str(v.string_index); }

  // The C# name a non-temp identifier reads or writes as. An auto-property's
  // backing slot resolves to the property, a member, property, or local folds to
  // its declared casing, and a setter's value parameter becomes C#'s `value`.
  std::string RefName(const std::string& raw) const {
    if (!value_alias_.empty() && IEquals(raw, value_alias_.c_str())) return "value";
    if (backing_) {
      auto it = backing_->find(raw);
      if (it != backing_->end()) return it->second;
    }
    auto it = local_case_.find(ToLower(raw));
    if (it != local_case_.end()) return it->second;
    return Sanitize(raw);
  }

  // The C# name a self-call resolves to: a field-collision rename wins, then the
  // function's declared casing, else the sanitised name.
  std::string SelfFn(const std::string& raw) const {
    if (fn_rename_) {
      auto it = fn_rename_->find(raw);
      if (it != fn_rename_->end()) return it->second;
    }
    if (case_fns_) {
      auto it = case_fns_->find(ToLower(raw));
      if (it != case_fns_->end()) return it->second;
    }
    return Sanitize(raw);
  }
  bool IsLocal(const std::string& name) const { return locals_.count(name) != 0; }
  bool IsTemp(const std::string& name) const {
    return name.size() >= 2 && name[0] == ':' && name[1] == ':' && IsLocal(name);
  }

  void BuildSymbolTables() {
    locals_.clear();
    // Seed the case-folding table from the object-level names, then let this
    // function's params and locals shadow them.
    local_case_ = case_names_ ? *case_names_ : std::unordered_map<std::string, std::string>{};
    for (const TypedName& p : fn_->params) {
      type_of_[pex_.Str(p.name)] = pex_.Str(p.type);
      local_case_[ToLower(pex_.Str(p.name))] = Sanitize(pex_.Str(p.name));
    }
    for (const TypedName& l : fn_->locals) {
      locals_.insert(pex_.Str(l.name));
      type_of_[pex_.Str(l.name)] = pex_.Str(l.type);
      local_case_[ToLower(pex_.Str(l.name))] = Sanitize(pex_.Str(l.name));
    }
  }

  void CountReads() {
    for (const Instruction& in : fn_->code) {
      const Roles r = RolesOf(in.op);
      for (int idx : r.reads) {
        if (idx < static_cast<int>(in.args.size()) &&
            in.args[idx].type == VariableData::Type::kIdentifier)
          read_count_[Name(in.args[idx])]++;
      }
      if (r.var_reads)
        for (const VariableData& a : in.var_args)
          if (a.type == VariableData::Type::kIdentifier) read_count_[Name(a)]++;
    }
  }

  std::string Read(const VariableData& v) {
    switch (v.type) {
      case VariableData::Type::kNone:    return "null";
      case VariableData::Type::kString:  return EscapeString(Name(v));
      case VariableData::Type::kInteger: return std::to_string(v.int_value);
      case VariableData::Type::kFloat:   return FormatFloat(v.float_value);
      case VariableData::Type::kBool:    return v.bool_value ? "true" : "false";
      case VariableData::Type::kIdentifier: {
        const std::string& n = Name(v);
        if (IEquals(n, "self")) return "this";
        auto it = temp_expr_.find(n);
        if (it != temp_expr_.end()) return it->second;
        return RefName(n);
      }
    }
    return "null";
  }

  std::string CallArgs(const Instruction& in) {
    std::string s;
    for (size_t i = 0; i < in.var_args.size(); ++i) {
      if (i) s += ", ";
      s += Read(in.var_args[i]);
    }
    return s;
  }

  std::string ElemType(const VariableData& dest) {
    if (harness()) return "dynamic";
    auto it = type_of_.find(Name(dest));
    std::string t = it != type_of_.end() ? it->second : "var";
    if (t.size() >= 2 && t.compare(t.size() - 2, 2, "[]") == 0) t.resize(t.size() - 2);
    return CsType(t);
  }

  // The Papyrus base type of an operand (lower-cased, array suffix stripped), or
  // "" if unknown.
  std::string SrcType(const VariableData& v) {
    std::string t;
    switch (v.type) {
      case VariableData::Type::kInteger: return "int";
      case VariableData::Type::kFloat:   return "float";
      case VariableData::Type::kBool:    return "bool";
      case VariableData::Type::kString:  return "string";
      case VariableData::Type::kNone:    return "none";
      case VariableData::Type::kIdentifier: {
        auto it = type_of_.find(Name(v));
        t = it != type_of_.end() ? ToLower(it->second) : "";
        break;
      }
    }
    if (t.size() >= 2 && t.compare(t.size() - 2, 2, "[]") == 0) t.resize(t.size() - 2);
    return t;
  }

  // Renders a Papyrus cast. The compiler inserts these for the implicit
  // conversions Papyrus allows but C# does not (int to bool, int to float,
  // anything to string), so dropping one would change an argument's type. The
  // destination's declared type is the target.
  std::string BuildCast(const VariableData& dest, const VariableData& src_op) {
    std::string src = Read(src_op);
    std::string raw;
    auto it = type_of_.find(Name(dest));
    if (it != type_of_.end()) raw = it->second;
    std::string tt = ToLower(raw);
    if (tt.size() >= 2 && tt.compare(tt.size() - 2, 2, "[]") == 0) tt.resize(tt.size() - 2);
    std::string st = SrcType(src_op);
    if (tt.empty() || tt == st) return src;
    if (tt == "bool") {
      if (st == "int" || st == "float") return "(" + src + " != 0)";
      if (st == "string") return "(" + src + " != \"\")";
      if (st == "bool") return src;
      return "(" + src + " != null)";
    }
    if (tt == "float") return "(float)(" + src + ")";
    if (tt == "int") return "(int)(" + src + ")";
    if (tt == "string") return "(\"\" + " + src + ")";
    return harness() ? src : "(" + CsType(raw) + ")(" + src + ")";  // object downcast
  }

  // The right-hand-side expression for a value-producing opcode, self-delimited
  // (binary forms parenthesised) so it can be substituted into another expression.
  std::string BuildExpr(const Instruction& in) {
    const auto& a = in.args;
    auto bin = [&](const char* op) {
      return "(" + Read(a[1]) + " " + op + " " + Read(a[2]) + ")";
    };
    switch (in.op) {
      case Op::kIAdd: case Op::kFAdd: return bin("+");
      case Op::kISub: case Op::kFSub: return bin("-");
      case Op::kIMul: case Op::kFMul: return bin("*");
      case Op::kIDiv: case Op::kFDiv: return bin("/");
      case Op::kIMod:                 return bin("%");
      case Op::kStrCat:               return bin("+");
      case Op::kCmpEq:                return bin("==");
      case Op::kCmpLt:                return bin("<");
      case Op::kCmpLe:                return bin("<=");
      case Op::kCmpGt:                return bin(">");
      case Op::kCmpGe:                return bin(">=");
      case Op::kNot:                  return "(!" + Read(a[1]) + ")";
      case Op::kINeg: case Op::kFNeg: return "(-" + Read(a[1]) + ")";
      case Op::kAssign:               return Read(a[1]);
      case Op::kCast:                 return BuildCast(a[0], a[1]);
      case Op::kCallMethod: {
        std::string tgt = Read(a[1]);
        std::string method = tgt == "this" ? SelfFn(Name(a[0])) : Sanitize(Name(a[0]));
        std::string call = method + "(" + CallArgs(in) + ")";
        if (tgt != "this") return tgt + "." + call;
        // A bare self-call may target a method inherited from a parent script. The
        // harness has no base type, so it routes through dynamic.
        return harness() ? "((dynamic)this)." + call : call;
      }
      case Op::kCallParent: {
        std::string call = SelfFn(Name(a[0])) + "(" + CallArgs(in) + ")";
        return harness() ? "((dynamic)this)." + call : "base." + call;
      }
      case Op::kCallStatic: {
        std::string type = CsType(Name(a[0]));
        std::string method = Sanitize(Name(a[1]));
        // The same global may be reached as Game.GetPlayer and game.getplayer; a
        // case-sensitive C# target cannot host both, so the harness funnels every
        // static call through one dynamic helper.
        if (harness()) {
          std::string args = CallArgs(in);
          return "__Native(\"" + type + "." + Name(a[1]) + "\"" +
                 (args.empty() ? "" : ", " + args) + ")";
        }
        if (sink()) {
          sink()->ref_types.insert(type);
          sink()->static_calls.insert(type + "." + method);
        }
        return type + "." + method + "(" + CallArgs(in) + ")";
      }
      case Op::kPropGet: {
        std::string tgt = Read(a[1]);
        return tgt == "this" ? RefName(Name(a[0])) : tgt + "." + Sanitize(Name(a[0]));
      }
      case Op::kArrayCreate:     return "new " + ElemType(a[0]) + "[" + Read(a[1]) + "]";
      case Op::kArrayLength:     return Read(a[1]) + ".Length";
      case Op::kArrayGetElement: return Read(a[1]) + "[" + Read(a[2]) + "]";
      case Op::kArrayFindElement:  return Read(a[0]) + ".Find(" + Read(a[2]) + ", " + Read(a[3]) + ")";
      case Op::kArrayRFindElement: return Read(a[0]) + ".RFind(" + Read(a[2]) + ", " + Read(a[3]) + ")";
      case Op::kIs: {
        std::string type = CsType(Name(a[2]));
        if (harness()) return "__Is(" + Read(a[1]) + ")";
        if (sink()) sink()->ref_types.insert(type);
        return "(" + Read(a[1]) + " is " + type + ")";
      }
      default:
        return "default /* unsupported opcode " + std::string(GetOpInfo(in.op).mnemonic) + " */";
    }
  }

  // Emits one non-branch instruction. A write to a temp is inlined into temp_expr_
  // (or materialised as a hoisted local when read more than once); a write to a
  // real variable, and the side-effecting statement opcodes, become statements.
  void EmitInstruction(const Instruction& in) {
    const auto& a = in.args;
    switch (in.op) {
      case Op::kNop:    return;
      case Op::kReturn:
        Line(a.empty() || a[0].type == VariableData::Type::kNone ? "return;"
                                                                 : "return " + Read(a[0]) + ";");
        return;
      case Op::kPropSet: {
        std::string tgt = Read(a[1]);
        std::string lhs = tgt == "this" ? RefName(Name(a[0])) : tgt + "." + Sanitize(Name(a[0]));
        Line(lhs + " = " + Read(a[2]) + ";");
        return;
      }
      case Op::kArraySetElement:
        Line(Read(a[0]) + "[" + Read(a[1]) + "] = " + Read(a[2]) + ";");
        return;
      default: break;
    }

    const Roles r = RolesOf(in.op);
    if (r.dest < 0) {
      if (in.op != Op::kNop) Line("// " + std::string(GetOpInfo(in.op).mnemonic));
      return;
    }
    const VariableData& dest = a[r.dest];
    const std::string& dname = Name(dest);
    std::string expr = BuildExpr(in);

    if (IsTemp(dname)) {
      int rc = read_count_[dname];
      // A literal staged through a temp inlines at every use: duplicating a
      // constant is harmless and avoids a `var _t = 5;` for each numeric argument.
      bool const_load = (in.op == Op::kAssign || in.op == Op::kCast) &&
                        a[1].type != VariableData::Type::kIdentifier;
      if (rc == 0) {
        if (HasSideEffect(in.op)) Line(expr + ";");
      } else if (rc == 1 || const_load) {
        temp_expr_[dname] = expr;
      } else {
        // Read more than once: materialise so a side-effecting call is not
        // duplicated. The local is named after the Papyrus slot, not the write,
        // because the compiler writes one slot on several branches (a short-circuit
        // result) and reads it at the merge, so every write must land in the same
        // variable. The declaration is hoisted to function scope for the same
        // cross-block reason.
        auto nit = mat_name_.find(dname);
        std::string local;
        if (nit != mat_name_.end()) {
          local = nit->second;
        } else {
          local = "_t" + std::to_string(mat_counter_++);
          mat_name_[dname] = local;
          auto it = type_of_.find(dname);
          materialized_.push_back({local, it != type_of_.end() ? it->second : "var"});
        }
        Line(local + " = " + expr + ";");
        temp_expr_[dname] = local;
      }
      return;
    }
    std::string lhs = RefName(dname);
    if (lhs == expr) return;  // a redundant no-op cast (x = x)
    Line(lhs + " = " + expr + ";");
  }

  // True when no branch crosses the boundary of [lo,hi): nothing inside jumps out
  // except to a target in `exits` or an enclosing loop edge, and nothing outside
  // jumps into the middle. Ranges that fail this guard go to the goto renderer.
  bool IsClean(int lo, int hi, const std::set<int>& exits) {
    const auto& code = fn_->code;
    auto is_loop_edge = [&](int t) {
      for (const auto& l : loops_)
        if (t == l.first || t == l.second) return true;
      return false;
    };
    for (int i = 0; i < static_cast<int>(code.size()); ++i) {
      if (!IsBranch(code[i].op)) continue;
      int tgt = i + JumpRel(code[i]);
      bool src_in = (i >= lo && i < hi);
      bool tgt_in = (tgt > lo && tgt < hi);
      // A forward jump to the region's end boundary is the merge point and stays
      // structurable, so it counts as in-range.
      if (src_in && !(tgt >= lo && tgt <= hi) && !exits.count(tgt) && !is_loop_edge(tgt))
        return false;
      if (!src_in && tgt_in) return false;
    }
    return true;
  }

  // C# break and continue only reach the innermost loop, so a jump to an outer
  // loop's edge cannot be structured.
  const std::pair<int, int>* InnerLoop() const {
    return loops_.empty() ? nullptr : &loops_.back();
  }

  // Emits [lo,hi) as structured C#. Returns false on any flow it cannot model, so
  // the caller falls back to the goto renderer for the whole function.
  bool EmitStructured(int lo, int hi) {
    const auto& code = fn_->code;
    int i = lo;
    while (i < hi) {
      int back_from = BackEdgeInto(i, hi);
      if (back_from >= 0) {
        if (!EmitLoop(i, back_from, hi)) return false;
        i = back_from + 1;
        continue;
      }
      const Instruction& in = code[i];
      if (!IsBranch(in.op)) {
        EmitInstruction(in);
        ++i;
        continue;
      }
      int target = i + JumpRel(in);
      const std::pair<int, int>* loop = InnerLoop();

      if (in.op == Op::kJmp) {
        // The compiler litters bodies with jumps to the next instruction (a
        // just-closed branch's merge point); they are no-ops once structured.
        if (target == i + 1) { ++i; continue; }
        if (loop && target == loop->second) { Line("break;"); ++i; continue; }
        if (loop && target == loop->first) { Line("continue;"); ++i; continue; }
        return false;
      }

      std::string cond = Read(in.args[0]);
      bool jmpf = (in.op == Op::kJmpF);
      auto neg = [&](const std::string& c) { return "!(" + c + ")"; };
      if (target == i + 1) { ++i; continue; }
      if (loop && target == loop->second) {
        Line("if (" + (jmpf ? neg(cond) : cond) + ") break;");
        ++i;
        continue;
      }
      if (loop && target == loop->first) {
        Line("if (" + (jmpf ? neg(cond) : cond) + ") continue;");
        ++i;
        continue;
      }

      // Forward conditional becomes if / if-else. JmpF runs the body when the
      // condition is true, JmpT is the inverse.
      if (target <= i || target > hi) return false;
      std::string if_cond = jmpf ? cond : neg(cond);

      // The then-block's last instruction is an unconditional forward jump over
      // the else block to the shared merge point.
      int then_end = target;
      int else_end = -1;
      if (target - 1 > i && code[target - 1].op == Op::kJmp) {
        int e = (target - 1) + JumpRel(code[target - 1]);
        if (e > target && e <= hi) {
          then_end = target - 1;
          else_end = e;
        }
      }
      if (else_end < 0) {
        if (!IsClean(i + 1, target, {target})) return false;
        Line("if (" + if_cond + ") {");
        ++indent_;
        if (!EmitStructured(i + 1, target)) return false;
        --indent_;
        Line("}");
        i = target;
        continue;
      }
      if (!IsClean(i + 1, then_end, {else_end}) || !IsClean(target, else_end, {else_end}))
        return false;
      Line("if (" + if_cond + ") {");
      ++indent_;
      if (!EmitStructured(i + 1, then_end)) return false;
      --indent_;
      Line("} else {");
      ++indent_;
      if (!EmitStructured(target, else_end)) return false;
      --indent_;
      Line("}");
      i = else_end;
    }
    return true;
  }

  // Largest index in [header,hi) that branches backward to header (the loop's
  // back-edge), or -1.
  int BackEdgeInto(int header, int hi) {
    const auto& code = fn_->code;
    int found = -1;
    for (int j = header + 1; j < hi; ++j)
      if (IsBranch(code[j].op) && j + JumpRel(code[j]) == header) found = j;
    return found;
  }

  // Emits the loop with the given header and back-edge as `while (true) { ... }`.
  // The exit test inside becomes `if (cond) break;` and the back-edge is the
  // implicit close, so arbitrarily placed exits and continues stay structured.
  bool EmitLoop(int header, int back, int hi) {
    int exit = back + 1;
    if (!IsClean(header, back + 1, {header, exit})) return false;

    Line("while (true) {");
    ++indent_;
    loops_.push_back({header, exit});
    bool ok = EmitStructured(header, back);
    loops_.pop_back();
    if (!ok) return false;
    --indent_;
    Line("}");
    return true;
  }

  // The always-correct rendering for flow the structurer rejects: one label per
  // jump target, jumps become gotos.
  void EmitGoto() {
    const auto& code = fn_->code;
    std::set<int> labels;
    for (int i = 0; i < static_cast<int>(code.size()); ++i)
      if (IsBranch(code[i].op)) labels.insert(i + JumpRel(code[i]));

    for (int i = 0; i < static_cast<int>(code.size()); ++i) {
      if (labels.count(i)) {
        int saved = indent_;
        indent_ = 0;
        Line("L" + std::to_string(i) + ":;");
        indent_ = saved;
      }
      const Instruction& in = code[i];
      if (IsBranch(in.op)) {
        int tgt = i + JumpRel(in);
        std::string lbl = "goto L" + std::to_string(tgt) + ";";
        if (in.op == Op::kJmp)
          Line(lbl);
        else if (in.op == Op::kJmpT)
          Line("if (" + Read(in.args[0]) + ") " + lbl);
        else
          Line("if (!(" + Read(in.args[0]) + ")) " + lbl);
        continue;
      }
      EmitInstruction(in);
    }
  }

  void Line(const std::string& s) {
    out_->append(static_cast<size_t>(indent_) * 4, ' ');
    out_->append(s);
    out_->push_back('\n');
  }

  bool harness() const { return opts_ && (opts_->compile_harness || opts_->runner); }
  HarnessSink* sink() const { return opts_ ? opts_->sink : nullptr; }

  const PexFile& pex_;
  const std::unordered_map<std::string, std::string>* backing_ = nullptr;
  const std::unordered_map<std::string, std::string>* fn_rename_ = nullptr;
  const std::unordered_map<std::string, std::string>* case_names_ = nullptr;
  const std::unordered_map<std::string, std::string>* case_fns_ = nullptr;
  std::unordered_map<std::string, std::string> local_case_;
  std::string value_alias_;
  const TranspileOptions* opts_ = nullptr;
  const Function* fn_ = nullptr;
  std::unordered_set<std::string> locals_;
  std::unordered_map<std::string, std::string> type_of_;
  std::unordered_map<std::string, std::string> temp_expr_;
  std::unordered_map<std::string, int> read_count_;
  std::string* out_ = nullptr;
  int indent_ = 0;
  int mat_counter_ = 0;
  std::vector<std::pair<int, int>> loops_;
  std::vector<std::pair<std::string, std::string>> materialized_;
  std::unordered_map<std::string, std::string> mat_name_;
};

}  // namespace

void DecompileFunction(const DecompileCtx& ctx, const Function& fn, std::string& out, int indent,
                       const std::string& value_alias) {
  Decompiler(ctx).Emit(fn, out, indent, value_alias);
}

}  // namespace rec::script::papyrus::detail
