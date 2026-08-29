// Assembles AVM1 action blocks by hand and asserts the ActionScript that comes
// back out, covering the expression rebuilder and every control-flow shape the
// structurer claims to recover.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <cstdio>

#include "components/swf/avm1.h"
#include "components/swf/decompile.h"

using namespace rx;

namespace {

int failures = 0;

void Check(bool condition, const char* what, const base::String& source) {
  if (condition)
    return;
  std::printf("FAIL: %s\n--- decompiled ---\n%s------------------\n", what,
              source.c_str());
  ++failures;
}

class Code {
 public:
  void Op(u8 code) { bytes_.push_back(code); }

  void PushString(const char* text) {
    base::Vector<u8> payload;
    payload.push_back(0);  // string
    for (const char* p = text; *p; ++p)
      payload.push_back(static_cast<u8>(*p));
    payload.push_back(0);
    Emit(swf::op::kPush, payload);
  }

  void PushInt(i32 value) {
    base::Vector<u8> payload;
    payload.push_back(7);  // int
    for (int i = 0; i < 4; ++i)
      payload.push_back(static_cast<u8>((static_cast<u32>(value) >> (i * 8)) & 0xff));
    Emit(swf::op::kPush, payload);
  }

  // Returns the offset of the two-byte operand so it can be patched once the
  // branch target is known.
  mem_size Branch(u8 code) {
    base::Vector<u8> payload;
    payload.push_back(0);
    payload.push_back(0);
    Emit(code, payload);
    return bytes_.size() - 2;
  }

  void PatchTo(mem_size operand, mem_size target) {
    const i32 delta = static_cast<i32>(target) - static_cast<i32>(operand + 2);
    bytes_[operand] = static_cast<u8>(delta & 0xff);
    bytes_[operand + 1] = static_cast<u8>((delta >> 8) & 0xff);
  }

  void Function(const char* name, const char* param, mem_size body_size) {
    base::Vector<u8> payload;
    for (const char* p = name; *p; ++p)
      payload.push_back(static_cast<u8>(*p));
    payload.push_back(0);
    const u16 params = param ? 1 : 0;
    payload.push_back(static_cast<u8>(params & 0xff));
    payload.push_back(static_cast<u8>(params >> 8));
    if (param) {
      for (const char* p = param; *p; ++p)
        payload.push_back(static_cast<u8>(*p));
      payload.push_back(0);
    }
    payload.push_back(static_cast<u8>(body_size & 0xff));
    payload.push_back(static_cast<u8>(body_size >> 8));
    Emit(swf::op::kDefineFunction, payload);
  }

  mem_size size() const { return bytes_.size(); }
  ByteSpan span() const { return ByteSpan{bytes_.data(), bytes_.size()}; }

 private:
  void Emit(u8 code, const base::Vector<u8>& payload) {
    bytes_.push_back(code);
    bytes_.push_back(static_cast<u8>(payload.size() & 0xff));
    bytes_.push_back(static_cast<u8>(payload.size() >> 8));
    for (mem_size i = 0; i < payload.size(); ++i)
      bytes_.push_back(payload[i]);
  }

  base::Vector<u8> bytes_;
};

bool Has(const base::String& text, const char* needle) {
  return text.find(needle) != base::String::npos;
}

void TestTrace() {
  Code c;
  c.PushString("hello");
  c.Op(swf::op::kTrace);
  c.Op(swf::op::kEnd);
  const base::String out = swf::Decompile(c.span());
  Check(Has(out, "trace(\"hello\");"), "trace of a literal", out);
}

void TestMemberAssignment() {
  Code c;
  c.PushString("obj");
  c.Op(swf::op::kGetVariable);
  c.PushString("field");
  c.PushInt(42);
  c.Op(swf::op::kSetMember);
  c.Op(swf::op::kEnd);
  const base::String out = swf::Decompile(c.span());
  Check(Has(out, "obj.field = 42;"), "member assignment", out);
}

void TestMethodCall() {
  // CallMethod takes the arguments deepest first, then the count, then the
  // object and the method name.
  Code c;
  c.PushString("arg");
  c.PushInt(1);
  c.PushString("target");
  c.Op(swf::op::kGetVariable);
  c.PushString("doThing");
  c.Op(swf::op::kCallMethod);
  c.Op(swf::op::kPop);
  c.Op(swf::op::kEnd);
  const base::String out = swf::Decompile(c.span());
  Check(Has(out, "target.doThing(\"arg\");"), "method call with one argument", out);
}

void TestArithmeticPrecedence() {
  Code c;
  c.PushString("out");
  c.PushInt(1);
  c.PushInt(2);
  c.PushInt(3);
  c.Op(swf::op::kMultiply);
  c.Op(swf::op::kAdd2);
  c.Op(swf::op::kSetVariable);
  c.Op(swf::op::kEnd);
  const base::String out = swf::Decompile(c.span());
  Check(Has(out, "out = 1 + 2 * 3;"), "precedence needs no parentheses", out);

  Code d;
  d.PushString("out");
  d.PushInt(1);
  d.PushInt(2);
  d.Op(swf::op::kAdd2);
  d.PushInt(3);
  d.Op(swf::op::kMultiply);
  d.Op(swf::op::kSetVariable);
  d.Op(swf::op::kEnd);
  const base::String parens = swf::Decompile(d.span());
  Check(Has(parens, "out = (1 + 2) * 3;"), "precedence adds parentheses", parens);
}

void TestIfElse() {
  // if (a) { trace("y") } else { trace("n") }
  Code c;
  c.PushString("a");
  c.Op(swf::op::kGetVariable);
  c.Op(swf::op::kNot);
  const mem_size to_else = c.Branch(swf::op::kIf);
  c.PushString("y");
  c.Op(swf::op::kTrace);
  const mem_size to_end = c.Branch(swf::op::kJump);
  c.PatchTo(to_else, c.size());
  c.PushString("n");
  c.Op(swf::op::kTrace);
  c.PatchTo(to_end, c.size());
  c.Op(swf::op::kEnd);

  const base::String out = swf::Decompile(c.span());
  Check(Has(out, "if (a) {"), "the if keeps the un-negated condition", out);
  Check(Has(out, "} else {"), "the else arm is recovered", out);
  Check(Has(out, "trace(\"y\");") && Has(out, "trace(\"n\");"), "both arms decompile",
        out);
}

void TestWhile() {
  // while (i) { trace("x"); }
  Code c;
  const mem_size top = c.size();
  c.PushString("i");
  c.Op(swf::op::kGetVariable);
  c.Op(swf::op::kNot);
  const mem_size to_end = c.Branch(swf::op::kIf);
  c.PushString("x");
  c.Op(swf::op::kTrace);
  const mem_size back = c.Branch(swf::op::kJump);
  c.PatchTo(back, top);
  c.PatchTo(to_end, c.size());
  c.Op(swf::op::kEnd);

  const base::String out = swf::Decompile(c.span());
  Check(Has(out, "while (i) {"), "the loop is recovered as a while", out);
  Check(Has(out, "trace(\"x\");"), "the loop body decompiles", out);
}

void TestLogicalOr() {
  // if (a || b) { trace("hit") }
  Code c;
  c.PushString("a");
  c.Op(swf::op::kGetVariable);
  c.Op(swf::op::kPushDuplicate);
  const mem_size join = c.Branch(swf::op::kIf);
  c.Op(swf::op::kPop);
  c.PushString("b");
  c.Op(swf::op::kGetVariable);
  c.PatchTo(join, c.size());
  c.Op(swf::op::kNot);
  const mem_size to_end = c.Branch(swf::op::kIf);
  c.PushString("hit");
  c.Op(swf::op::kTrace);
  c.PatchTo(to_end, c.size());
  c.Op(swf::op::kEnd);

  const base::String out = swf::Decompile(c.span());
  Check(Has(out, "if (a || b) {"), "the short-circuit stays an expression", out);
}

void TestFunction() {
  Code body;
  body.PushString("v");
  body.Op(swf::op::kTrace);

  Code c;
  c.Function("greet", "who", body.size());
  const ByteSpan tail = body.span();
  // The body follows the action in the byte stream.
  base::Vector<u8> whole;
  const ByteSpan head = c.span();
  for (mem_size i = 0; i < head.size(); ++i)
    whole.push_back(head[i]);
  for (mem_size i = 0; i < tail.size(); ++i)
    whole.push_back(tail[i]);
  whole.push_back(swf::op::kEnd);

  const base::String out = swf::Decompile(ByteSpan{whole.data(), whole.size()});
  Check(Has(out, "function greet(who) {"), "the function signature", out);
  Check(Has(out, "trace(\"v\");"), "the function body decompiles", out);
}

void TestDisassembly() {
  Code c;
  c.PushString("x");
  c.Op(swf::op::kTrace);
  c.Op(swf::op::kEnd);
  const base::String listing = swf::Disassembly(c.span());
  Check(Has(listing, "Push"), "the listing names the push", listing);
  Check(Has(listing, "Trace"), "the listing names the trace", listing);
  Check(swf::OpName(swf::op::kCallMethod) == "CallMethod", "opcode names resolve",
        listing);
}

}  // namespace

int main() {
  TestTrace();
  TestMemberAssignment();
  TestMethodCall();
  TestArithmeticPrecedence();
  TestIfElse();
  TestWhile();
  TestLogicalOr();
  TestFunction();
  TestDisassembly();
  std::printf("avm1test: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
