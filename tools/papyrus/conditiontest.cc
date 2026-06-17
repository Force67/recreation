// conditiontest: deterministic checks for the native condition IR, its
// AND-of-ORs evaluator, and the CTDA transpiler. No game data needed, so it
// runs in the ctest gate alongside questtest.

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/types.h"
#include "quest/condition.h"
#include "quest/ctda.h"

using namespace rec;
using namespace rec::quest;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Builds a 32-byte (SE) CTDA payload from its fields, so the parser sees a
// realistic record. operator_bits go in the top 3 bits, flags in the low 5.
std::vector<u8> MakeCtda(u8 operator_bits, u8 flags, float value, u16 function,
                         u32 param1, u32 run_on, u32 reference) {
  std::vector<u8> b(32, 0);
  b[0] = static_cast<u8>((operator_bits << 5) | (flags & 0x1F));
  std::memcpy(b.data() + 4, &value, 4);
  std::memcpy(b.data() + 8, &function, 2);
  std::memcpy(b.data() + 12, &param1, 4);
  std::memcpy(b.data() + 20, &run_on, 4);
  std::memcpy(b.data() + 24, &reference, 4);
  return b;
}

ByteSpan Span(const std::vector<u8>& v) { return ByteSpan(v.data(), v.size()); }

// A context that reports a fixed stage for one quest and a fixed raw result, so
// tests can exercise both the typed and the fallback dispatch paths.
class MockContext : public ConditionContext {
 public:
  float stage = 0.0f;
  u64 stage_quest = 0;
  float raw = 0.0f;
  float global = 0.0f;

  float GetStage(u64 quest) const override { return quest == stage_quest ? stage : -1.0f; }
  float GetGlobal(u64) const override { return global; }
  float EvalRaw(const Comparison&) const override { return raw; }
};

void TestParse() {
  std::printf("ParseCtda:\n");
  // GetStage(quest 0x1234) >= 20, OR-linked with the next condition.
  auto bytes = MakeCtda(/*op=*/3 /*>=*/, /*flags=*/0x01 /*OR*/, /*value=*/20.0f,
                        /*function=*/58 /*GetStage*/, /*param1=*/0x1234,
                        /*run_on=*/0, /*reference=*/0);
  Comparison c;
  Check("parses a 32-byte CTDA", ParseCtda(Span(bytes), &c));
  Check("operator decoded", c.op == CompareOp::kGreaterOrEqual);
  Check("or_next flag decoded", c.or_next);
  Check("function index preserved", c.raw_function == 58);
  Check("GetStage mapped to native func", c.func == Func::kGetStage);
  Check("param1 decoded", c.param1 == 0x1234);
  Check("value decoded", c.value == 20.0f);
  Check("no global when flag clear", c.global == 0);

  // "Use global" flag: the value field is a GLOB form id, not a float.
  auto gbytes = MakeCtda(0, 0x04, 0.0f, 58, 0, 0, 0);
  const u32 glob_id = 0xAABBCCDD;
  std::memcpy(gbytes.data() + 4, &glob_id, 4);
  Comparison g;
  ParseCtda(Span(gbytes), &g);
  Check("use-global flag captures GLOB id", g.global == 0xAABBCCDD);

  // Unmapped function stays raw but keeps its id.
  auto rbytes = MakeCtda(0, 0, 0.0f, 9999, 0, 0, 0);
  Comparison r;
  ParseCtda(Span(rbytes), &r);
  Check("unknown function stays kRaw", r.func == Func::kRaw && r.raw_function == 9999);

  // Too short is rejected.
  std::vector<u8> tiny(10, 0);
  Comparison junk;
  Check("rejects undersized payload", !ParseCtda(Span(tiny), &junk));
}

Comparison Cmp(Func f, CompareOp op, float value, bool or_next) {
  Comparison c;
  c.func = f;
  c.op = op;
  c.value = value;
  c.or_next = or_next;
  return c;
}

void TestEvaluate() {
  std::printf("Evaluate:\n");
  MockContext ctx;
  ctx.stage_quest = 7;
  ctx.stage = 30.0f;

  Check("empty list is true", Evaluate(ConditionList{}, ctx));

  // Single satisfied / unsatisfied comparison.
  ConditionList ge;
  ge.comparisons.push_back(Cmp(Func::kGetStage, CompareOp::kGreaterOrEqual, 20.0f, false));
  ge.comparisons[0].param1 = 7;
  Check("GetStage 30 >= 20 passes", Evaluate(ge, ctx));
  ge.comparisons[0].value = 40.0f;
  Check("GetStage 30 >= 40 fails", !Evaluate(ge, ctx));

  // AND: two separate groups, both must hold.
  ConditionList both;
  both.comparisons.push_back(Cmp(Func::kGetStage, CompareOp::kGreaterOrEqual, 20.0f, false));
  both.comparisons.push_back(Cmp(Func::kGetStage, CompareOp::kLess, 100.0f, false));
  for (auto& c : both.comparisons) c.param1 = 7;
  Check("AND of two passing groups passes", Evaluate(both, ctx));
  both.comparisons[1].value = 25.0f;  // 30 < 25 is false
  Check("AND fails when one group fails", !Evaluate(both, ctx));

  // OR: one group of two, either may hold.
  ConditionList either;
  either.comparisons.push_back(Cmp(Func::kGetStage, CompareOp::kEqual, 999.0f, true));
  either.comparisons.push_back(Cmp(Func::kGetStage, CompareOp::kEqual, 30.0f, false));
  for (auto& c : either.comparisons) c.param1 = 7;
  Check("OR passes when the second disjunct holds", Evaluate(either, ctx));
  either.comparisons[1].value = 31.0f;  // neither matches now
  Check("OR fails when no disjunct holds", !Evaluate(either, ctx));

  // Raw fallback flows through EvalRaw.
  ctx.raw = 5.0f;
  ConditionList raw;
  raw.comparisons.push_back(Cmp(Func::kRaw, CompareOp::kEqual, 5.0f, false));
  Check("raw comparison uses EvalRaw", Evaluate(raw, ctx));

  // Global right-hand side.
  ctx.global = 30.0f;
  ConditionList glob;
  glob.comparisons.push_back(Cmp(Func::kGetStage, CompareOp::kEqual, 0.0f, false));
  glob.comparisons[0].param1 = 7;
  glob.comparisons[0].global = 0x10;  // compare against GetGlobal(), which is 30
  Check("global right-hand side compares against GetGlobal", Evaluate(glob, ctx));
}

}  // namespace

int main() {
  std::printf("conditiontest\n");
  TestParse();
  TestEvaluate();
  if (g_failures) {
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
