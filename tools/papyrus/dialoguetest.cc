// dialoguetest: checks INFO response parsing and the INFO VMAD fragment parser
// (the fragment is what advances a quest when a line plays). No game data.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bethesda/record.h"
#include "bethesda/script_attachment.h"
#include "core/types.h"
#include "dialogue/dialogue.h"

using namespace rec;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Backing store for synthetic subrecord spans.
struct Buffers {
  std::vector<std::vector<u8>> store;
  ByteSpan Bytes(const void* p, size_t n) {
    auto& b = store.emplace_back(n);
    if (n) std::memcpy(b.data(), p, n);
    return ByteSpan(b.data(), b.size());
  }
  ByteSpan Str(const char* s) { return Bytes(s, std::strlen(s) + 1); }
};

void AppendU16(std::vector<u8>& v, u16 x) {
  v.push_back(static_cast<u8>(x));
  v.push_back(static_cast<u8>(x >> 8));
}
void AppendWStr(std::vector<u8>& v, const char* s) {
  u16 len = static_cast<u16>(std::strlen(s));
  AppendU16(v, len);
  v.insert(v.end(), s, s + len);
}

// An INFO VMAD with no scripts and a single begin fragment.
std::vector<u8> MakeInfoVmad(const char* script, const char* function) {
  std::vector<u8> v;
  AppendU16(v, 5);  // version
  AppendU16(v, 2);  // object format
  AppendU16(v, 0);  // script count
  v.push_back(2);   // fragment version
  v.push_back(0x01);  // flags: begin fragment present
  AppendWStr(v, "");  // shared file name
  v.push_back(0);     // begin: unknown byte
  AppendWStr(v, script);
  AppendWStr(v, function);
  return v;
}

void Add(bethesda::Record& r, u32 type, ByteSpan data) {
  bethesda::Subrecord sub;
  sub.type = type;
  sub.data = data;
  r.subrecords.push_back(std::move(sub));
}

// A 32-byte (SE) CTDA payload: operator in the top 3 control bits, flags in the
// low 5; comparison value; function index; param1.
std::vector<u8> MakeCtda(u8 op_bits, u8 flags, float value, u16 function, u32 param1) {
  std::vector<u8> b(32, 0);
  b[0] = static_cast<u8>((op_bits << 5) | (flags & 0x1F));
  std::memcpy(b.data() + 4, &value, 4);
  std::memcpy(b.data() + 8, &function, 2);
  std::memcpy(b.data() + 12, &param1, 4);
  return b;
}

// A ConditionContext that knows only the quest's stage, so GetStage gates can be
// driven deterministically.
struct StageContext : quest::ConditionContext {
  float stage = 0.0f;
  float GetStage(rec::u64) const override { return stage; }
};

void TestInfoFragments() {
  std::puts("info vmad fragments:");
  std::vector<u8> vmad = MakeInfoVmad("TIF__010A1234", "Fragment_0");
  bethesda::ScriptAttachment att;
  bethesda::InfoFragments frags;
  bool ok = bethesda::ParseInfoFragments(ByteSpan(vmad.data(), vmad.size()), &att, &frags);
  Check("parse ok", ok);
  Check("no user scripts", att.scripts.empty());
  Check("begin script", frags.begin.script_name == "TIF__010A1234");
  Check("begin function", frags.begin.function == "Fragment_0");
  Check("no end fragment", frags.end.script_name.empty());

  // A truncated tail must not crash and must leave fragments empty.
  std::vector<u8> truncated(vmad.begin(), vmad.begin() + 8);
  bethesda::InfoFragments frags2;
  bethesda::ParseInfoFragments(ByteSpan(truncated.data(), truncated.size()), &att, &frags2);
  Check("truncated tail -> no begin fragment", frags2.begin.script_name.empty());
}

void TestInfoRecord() {
  std::puts("info record:");
  Buffers buf;
  std::vector<u8> vmad = MakeInfoVmad("TIF__010A1234", "Fragment_0");
  bethesda::Record info;
  Add(info, FourCc('R', 'N', 'A', 'M'), buf.Str("Where are we going?"));
  Add(info, FourCc('N', 'A', 'M', '1'), buf.Str("Follow me to the keep."));
  Add(info, FourCc('V', 'M', 'A', 'D'), buf.Bytes(vmad.data(), vmad.size()));

  dialogue::Response r = dialogue::ParseInfoRecord(info, 0x000a5678ull, "fallback prompt", nullptr);
  Check("info handle", r.info == 0x000a5678ull);
  Check("player line from RNAM", r.player_line == "Where are we going?");
  Check("npc line from NAM1", r.npc_line == "Follow me to the keep.");
  Check("fragment script parsed", r.fragment_script == "TIF__010A1234");
  Check("fragment function parsed", r.fragment_function == "Fragment_0");

  // No RNAM falls back to the topic text.
  bethesda::Record info2;
  Add(info2, FourCc('N', 'A', 'M', '1'), buf.Str("Hello."));
  dialogue::Response r2 = dialogue::ParseInfoRecord(info2, 1, "Greeting", nullptr);
  Check("player line falls back to topic", r2.player_line == "Greeting");
  Check("no fragment without vmad", r2.fragment_script.empty());
}

void TestInfoConditions() {
  std::puts("info conditions:");
  Buffers buf;
  bethesda::Record info;
  Add(info, FourCc('N', 'A', 'M', '1'), buf.Str("Quest words."));
  // GetStage(0x1234) >= 10  AND  GetStage(0x1234) < 100
  std::vector<u8> c1 = MakeCtda(3 /* >= */, 0x00, 10.0f, 58, 0x1234);
  std::vector<u8> c2 = MakeCtda(4 /* < */, 0x00, 100.0f, 58, 0x1234);
  Add(info, FourCc('C', 'T', 'D', 'A'), buf.Bytes(c1.data(), c1.size()));
  Add(info, FourCc('C', 'T', 'D', 'A'), buf.Bytes(c2.data(), c2.size()));

  dialogue::Response r = dialogue::ParseInfoRecord(info, 5, "", nullptr);
  Check("two conditions parsed", r.conditions.comparisons.size() == 2);
  Check("condition func is GetStage", r.conditions.comparisons[0].func == quest::Func::kGetStage);
  Check("param is the raw quest form id (resolved later by ParseTopic)",
        r.conditions.comparisons[0].param1 == 0x1234);

  StageContext ctx;
  ctx.stage = 5.0f;
  Check("below range -> unavailable", !dialogue::ResponseAvailable(r, ctx));
  ctx.stage = 50.0f;
  Check("in range -> available", dialogue::ResponseAvailable(r, ctx));
  ctx.stage = 150.0f;
  Check("above range -> unavailable", !dialogue::ResponseAvailable(r, ctx));

  // A response with no conditions is always available.
  bethesda::Record plain;
  Add(plain, FourCc('N', 'A', 'M', '1'), buf.Str("Hi."));
  dialogue::Response r2 = dialogue::ParseInfoRecord(plain, 6, "", nullptr);
  Check("no conditions -> available", dialogue::ResponseAvailable(r2, ctx));
}

quest::ConditionList StageCond(quest::CompareOp op, f32 value) {
  quest::ConditionList c;
  quest::Comparison cmp;
  cmp.func = quest::Func::kGetStage;
  cmp.op = op;
  cmp.value = value;
  c.comparisons.push_back(cmp);
  return c;
}

void TestAvailableResponses() {
  std::puts("available responses:");
  StageContext ctx;
  ctx.stage = 30.0f;

  dialogue::Topic t1;
  dialogue::Response r;
  r.player_line = "always";
  t1.responses.push_back(r);  // no conditions -> always available
  r = dialogue::Response{};
  r.player_line = "ge20";
  r.conditions = StageCond(quest::CompareOp::kGreaterOrEqual, 20.0f);  // 30>=20 ok
  t1.responses.push_back(r);
  r = dialogue::Response{};
  r.player_line = "ge40";
  r.conditions = StageCond(quest::CompareOp::kGreaterOrEqual, 40.0f);  // 30>=40 no
  t1.responses.push_back(r);

  dialogue::Topic t2;
  r = dialogue::Response{};
  r.player_line = "lt100";
  r.conditions = StageCond(quest::CompareOp::kLess, 100.0f);  // 30<100 ok
  t2.responses.push_back(r);

  std::vector<dialogue::Topic> topics{t1, t2};
  std::vector<dialogue::Response> avail = dialogue::AvailableResponses(topics, ctx);
  Check("3 of 4 responses available", avail.size() == 3);
  Check("flattened in topic then response order",
        avail.size() == 3 && avail[0].player_line == "always" && avail[1].player_line == "ge20" &&
            avail[2].player_line == "lt100");
}

}  // namespace

int main() {
  TestInfoFragments();
  TestInfoRecord();
  TestInfoConditions();
  TestAvailableResponses();
  if (g_failures == 0) {
    std::puts("dialogue: all checks passed");
    return 0;
  }
  std::printf("dialogue: %d checks FAILED\n", g_failures);
  return 1;
}
