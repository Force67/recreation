// scenetest: checks the SCEN scene parser. The deterministic part builds a
// synthetic MQ101-shaped scene and runs in the ctest gate (no game data). With
//
//   scenetest <data_dir>
//
// it additionally loads the real record store and dumps MQ101's SCEN scenes
// (owning quest 0x0003372b), which is how the parser was validated against the
// Helgen intro/escort scenes.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "bethesda/script_attachment.h"
#include "core/types.h"
#include "quest/scene_record.h"

using namespace rx;
using namespace rx::quest;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Backing store for the synthetic record's subrecord spans.
struct Buffers {
  std::vector<std::vector<u8>> store;
  ByteSpan Bytes(const void* p, size_t n) {
    auto& b = store.emplace_back(n);
    if (n) std::memcpy(b.data(), p, n);
    return ByteSpan(b.data(), b.size());
  }
  ByteSpan Empty() { return Bytes(nullptr, 0); }
  ByteSpan Str(const char* s) { return Bytes(s, std::strlen(s) + 1); }
  ByteSpan U16(u16 v) { return Bytes(&v, sizeof(v)); }
  ByteSpan U8(u8 v) { return Bytes(&v, sizeof(v)); }
  ByteSpan U32(u32 v) { return Bytes(&v, sizeof(v)); }
  ByteSpan I32(i32 v) { return Bytes(&v, sizeof(v)); }
  ByteSpan F32(f32 v) { return Bytes(&v, sizeof(v)); }
};

void Add(bethesda::Record& r, u32 type, ByteSpan data) {
  bethesda::Subrecord sub;
  sub.type = type;
  sub.data = data;
  r.subrecords.push_back(std::move(sub));
}

// Flat little-endian appenders for hand-building a VMAD byte stream.
void AppendU16(std::vector<u8>& v, u16 x) {
  v.push_back(static_cast<u8>(x));
  v.push_back(static_cast<u8>(x >> 8));
}
void AppendU32(std::vector<u8>& v, u32 x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
}
void AppendWStr(std::vector<u8>& v, const char* s) {
  u16 len = static_cast<u16>(std::strlen(s));
  AppendU16(v, len);
  v.insert(v.end(), s, s + len);
}

// A SCEN VMAD with no user scripts, a begin + end scene fragment, and one
// phase-end fragment for phase 4. Mirrors the SSE fragment layout the parser
// reads (version, flags, shared file name, begin/end, phase count, entries).
std::vector<u8> MakeSceneVmad() {
  std::vector<u8> v;
  AppendU16(v, 5);    // script section version
  AppendU16(v, 2);    // object format
  AppendU16(v, 0);    // script count
  v.push_back(2);     // fragment version
  v.push_back(0x03);  // flags: begin + end present
  AppendWStr(v, "SF_Test");  // shared file name
  v.push_back(0);            // begin: unknown
  AppendWStr(v, "SF_Test");
  AppendWStr(v, "Fragment_Begin");
  v.push_back(0);            // end: unknown
  AppendWStr(v, "SF_Test");
  AppendWStr(v, "Fragment_End");
  AppendU16(v, 1);           // phase count
  v.push_back(2);            // phase fragment kind: 2 = phase-end
  AppendU32(v, 4);           // phase number
  v.push_back(0);            // unknown
  AppendWStr(v, "SF_Test");
  AppendWStr(v, "Fragment_P4");
  return v;
}

// Builds an MQ101KeepEnemyScene-shaped SCEN: scene flags, two HNAM-bracketed
// phases (the second has a completion CTDA), two actor aliases, and two
// dialogue actions, then the owning-quest PNAM trailer.
bethesda::Record MakeScene(Buffers& buf) {
  bethesda::Record r;
  Add(r, FourCc('E', 'D', 'I', 'D'), buf.Str("MQ101KeepEnemyScene2A"));
  Add(r, FourCc('F', 'N', 'A', 'M'), buf.U32(4));  // scene flags

  // Phase 0: HNAM ... HNAM, no conditions.
  Add(r, FourCc('H', 'N', 'A', 'M'), buf.Empty());
  Add(r, FourCc('N', 'A', 'M', '0'), buf.U8(0));
  Add(r, FourCc('N', 'E', 'X', 'T'), buf.Empty());  // end of begin conditions
  Add(r, FourCc('N', 'E', 'X', 'T'), buf.Empty());  // end of completion conditions
  Add(r, FourCc('W', 'N', 'A', 'M'), buf.U32(200));
  Add(r, FourCc('H', 'N', 'A', 'M'), buf.Empty());

  // Phase 1: a completion CTDA sits between the two NEXT separators.
  u8 ctda[32] = {0};
  Add(r, FourCc('H', 'N', 'A', 'M'), buf.Empty());
  Add(r, FourCc('N', 'A', 'M', '0'), buf.U8(0));
  Add(r, FourCc('N', 'E', 'X', 'T'), buf.Empty());  // end of begin conditions
  Add(r, FourCc('C', 'T', 'D', 'A'), buf.Bytes(ctda, sizeof(ctda)));
  Add(r, FourCc('N', 'E', 'X', 'T'), buf.Empty());  // end of completion conditions
  Add(r, FourCc('W', 'N', 'A', 'M'), buf.U32(200));
  Add(r, FourCc('H', 'N', 'A', 'M'), buf.Empty());

  // Actors: two scene aliases.
  Add(r, FourCc('A', 'L', 'I', 'D'), buf.I32(84));
  Add(r, FourCc('L', 'N', 'A', 'M'), buf.U32(0));
  Add(r, FourCc('D', 'N', 'A', 'M'), buf.U32(26));
  Add(r, FourCc('A', 'L', 'I', 'D'), buf.I32(85));
  Add(r, FourCc('L', 'N', 'A', 'M'), buf.U32(0));
  Add(r, FourCc('D', 'N', 'A', 'M'), buf.U32(26));

  // Action 0: dialogue by alias 84, DIAL topic 0x000F5BAD, emotion + delays.
  Add(r, FourCc('A', 'N', 'A', 'M'), buf.U16(0));  // type 0 = dialogue
  Add(r, FourCc('N', 'A', 'M', '0'), buf.U8(0));
  Add(r, FourCc('A', 'L', 'I', 'D'), buf.I32(84));
  Add(r, FourCc('I', 'N', 'A', 'M'), buf.U32(1));
  Add(r, FourCc('S', 'N', 'A', 'M'), buf.I32(0));
  Add(r, FourCc('E', 'N', 'A', 'M'), buf.I32(0));
  Add(r, FourCc('D', 'A', 'T', 'A'), buf.U32(0x000F5BAD));  // DIAL topic
  Add(r, FourCc('H', 'T', 'I', 'D'), buf.I32(-1));
  Add(r, FourCc('D', 'M', 'A', 'X'), buf.F32(10.0f));
  Add(r, FourCc('D', 'M', 'I', 'N'), buf.F32(1.0f));
  Add(r, FourCc('D', 'E', 'M', 'O'), buf.U32(3));
  Add(r, FourCc('D', 'E', 'V', 'A'), buf.I32(50));
  Add(r, FourCc('A', 'N', 'A', 'M'), buf.Empty());  // close action 0

  // Action 1: package by alias 85, PACK ref 0x000F5BAC, spans phases 1..1.
  Add(r, FourCc('A', 'N', 'A', 'M'), buf.U16(1));  // type 1 = package
  Add(r, FourCc('N', 'A', 'M', '0'), buf.U8(0));
  Add(r, FourCc('A', 'L', 'I', 'D'), buf.I32(85));
  Add(r, FourCc('I', 'N', 'A', 'M'), buf.U32(2));
  Add(r, FourCc('P', 'N', 'A', 'M'), buf.U32(0x000F5BAC));  // package ref
  Add(r, FourCc('S', 'N', 'A', 'M'), buf.I32(1));
  Add(r, FourCc('E', 'N', 'A', 'M'), buf.I32(1));
  Add(r, FourCc('A', 'N', 'A', 'M'), buf.Empty());  // close action 1

  // Action 2: timer by alias 84, start phase 0, end phase 1, then a second SNAM
  // holding the 5s wait duration.
  Add(r, FourCc('A', 'N', 'A', 'M'), buf.U16(2));  // type 2 = timer
  Add(r, FourCc('N', 'A', 'M', '0'), buf.U8(0));
  Add(r, FourCc('A', 'L', 'I', 'D'), buf.I32(84));
  Add(r, FourCc('I', 'N', 'A', 'M'), buf.U32(3));
  Add(r, FourCc('S', 'N', 'A', 'M'), buf.I32(0));    // start phase
  Add(r, FourCc('E', 'N', 'A', 'M'), buf.I32(1));    // end phase
  Add(r, FourCc('S', 'N', 'A', 'M'), buf.F32(5.0f));  // timer duration
  Add(r, FourCc('A', 'N', 'A', 'M'), buf.Empty());   // close action 2

  // Trailer: owning quest (scene-level PNAM), action counter, version data.
  Add(r, FourCc('P', 'N', 'A', 'M'), buf.U32(0x0003372b));  // MQ101
  Add(r, FourCc('I', 'N', 'A', 'M'), buf.U32(2));
  return r;
}

void TestParse() {
  std::puts("scene parse (synthetic):");
  Buffers buf;
  bethesda::Record r = MakeScene(buf);
  // No record store: form ids stay raw, which is enough to assert layout.
  SceneDef def = ParseSceneRecord(0xabc123ull, r, nullptr);

  Check("handle preserved", def.handle == 0xabc123ull);
  Check("scene flags", def.flags == 4);
  Check("owning quest raw (MQ101)", def.quest == 0x0003372b);

  Check("two actors", def.actors.size() == 2);
  Check("actor 0 alias 84", !def.actors.empty() && def.actors[0].alias == 84);
  Check("actor 1 alias/flags2", def.actors.size() == 2 && def.actors[1].alias == 85 &&
                                    def.actors[1].flags2 == 26);

  Check("two phases", def.phases.size() == 2);
  Check("phase 0 index 0, no conditions",
        def.phases.size() == 2 && def.phases[0].index == 0 && def.phases[0].begin.empty() &&
            def.phases[0].completion.empty());
  Check("phase 1 has one completion CTDA",
        def.phases.size() == 2 && def.phases[1].index == 1 && def.phases[1].begin.empty() &&
            def.phases[1].completion.size() == 1 && def.phases[1].completion[0].ctda.size() == 32);

  Check("three actions", def.actions.size() == 3);
  if (def.actions.size() == 3) {
    const SceneActionDef& a0 = def.actions[0];
    Check("action 0 is dialogue", a0.kind == SceneActionDef::Kind::kDialogue);
    Check("action 0 actor alias 84", a0.actor_alias == 84);
    Check("action 0 topic raw", a0.topic == 0x000F5BAD);
    Check("action 0 delays", a0.delay_min == 1.0f && a0.delay_max == 10.0f);
    Check("action 0 emotion", a0.emotion_type == 3 && a0.emotion_value == 50);
    Check("action 0 head-track none", a0.head_track_alias == -1);
    Check("action 0 package empty", a0.package == 0);

    const SceneActionDef& a1 = def.actions[1];
    Check("action 1 is package", a1.kind == SceneActionDef::Kind::kPackage);
    Check("action 1 actor alias 85", a1.actor_alias == 85);
    Check("action 1 package raw", a1.package == 0x000F5BAC);
    Check("action 1 phase window 1..1", a1.start_phase == 1 && a1.end_phase == 1);
    Check("action 1 no topic", a1.topic == 0);

    const SceneActionDef& a2 = def.actions[2];
    Check("action 2 is timer", a2.kind == SceneActionDef::Kind::kTimer);
    Check("action 2 phase window 0..1", a2.start_phase == 0 && a2.end_phase == 1);
    Check("action 2 timer seconds (second SNAM)", a2.timer_seconds == 5.0f);
  }
}

void TestEmpty() {
  std::puts("scene parse (empty/sparse):");
  bethesda::Record r;  // no subrecords at all
  SceneDef def = ParseSceneRecord(7, r, nullptr);
  Check("empty scene does not crash", def.handle == 7 && def.phases.empty() &&
                                          def.actors.empty() && def.actions.empty());
}

void TestFragments() {
  std::puts("scene vmad fragments:");
  std::vector<u8> vmad = MakeSceneVmad();
  bethesda::ScriptAttachment att;
  bethesda::SceneFragments frags;
  bool ok = bethesda::ParseSceneFragments(ByteSpan(vmad.data(), vmad.size()), &att, &frags);
  Check("parse ok", ok);
  Check("no user scripts", att.scripts.empty());
  Check("scene begin fragment", frags.begin.function == "Fragment_Begin");
  Check("scene end fragment", frags.end.function == "Fragment_End");
  Check("one phase fragment", frags.phases.size() == 1);
  if (frags.phases.size() == 1) {
    Check("phase number 4", frags.phases[0].phase == 4);
    Check("phase fragment is an end fragment", !frags.phases[0].on_begin);
    Check("phase fragment function", frags.phases[0].fragment.function == "Fragment_P4");
  }

  // A truncated tail must not crash and must leave the fragments empty.
  std::vector<u8> truncated(vmad.begin(), vmad.begin() + 10);
  bethesda::SceneFragments frags2;
  bethesda::ParseSceneFragments(ByteSpan(truncated.data(), truncated.size()), &att, &frags2);
  Check("truncated tail -> no fragments",
        frags2.begin.script_name.empty() && frags2.phases.empty());
}

// Dumps the real MQ101 SCEN scenes when a data dir is given. Validation only;
// not part of the deterministic gate.
int DumpReal(const std::string& data_dir) {
  using namespace rx::bethesda;
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) {
    std::printf("failed to load records\n");
    return 1;
  }

  constexpr rx::u64 kMq101 = 0x0003372bull;  // MQ101 owning quest handle (Skyrim.esm)
  int scenes = 0;
  records.EachOfType(FourCc('S', 'C', 'E', 'N'),
                     [&](GlobalFormId id, const RecordStore::StoredRecord&) {
                       Record rec;
                       if (!records.Parse(id, &rec)) return;
                       SceneDef def = ParseSceneRecord(id.packed(), rec, &records);
                       // Resolve MQ101's handle for the load order to compare.
                       GlobalFormId mq = records.ResolveFrom(RawFormId{kMq101}, id.plugin);
                       if (def.quest != mq.packed() && def.quest != kMq101) return;
                       ++scenes;
                       std::printf("SCEN %04x:%06x  %s\n", id.plugin, id.local_id,
                                   rec.GetString(FourCc('E', 'D', 'I', 'D')).c_str());
                       std::printf("  flags=%u  quest=%llx  actors=%zu  phases=%zu  actions=%zu\n",
                                   def.flags, (unsigned long long)def.quest, def.actors.size(),
                                   def.phases.size(), def.actions.size());
                       for (const SceneActionDef& a : def.actions) {
                         const char* k = a.kind == SceneActionDef::Kind::kDialogue  ? "dialogue"
                                         : a.kind == SceneActionDef::Kind::kPackage  ? "package"
                                         : a.kind == SceneActionDef::Kind::kTimer    ? "timer"
                                                                                     : "unknown";
                         std::printf("    action %-8s alias=%d phases=%d..%d topic=%llx pkg=%llx\n",
                                     k, a.actor_alias, a.start_phase, a.end_phase,
                                     (unsigned long long)a.topic, (unsigned long long)a.package);
                       }
                       // Scene Papyrus fragments: the scene begin/end and per-phase
                       // functions that call SetStage to drive the journal.
                       for (const Subrecord& s : rec.subrecords) {
                         if (s.type != FourCc('V', 'M', 'A', 'D')) continue;
                         ScriptAttachment att;
                         SceneFragments frags;
                         if (!ParseSceneFragments(s.data, &att, &frags)) {
                           std::printf("  frags PARSE FAILED\n");
                           ++g_failures;
                           break;
                         }
                         std::printf("  frags begin='%s' end='%s' phases=%zu\n",
                                     frags.begin.function.c_str(), frags.end.function.c_str(),
                                     frags.phases.size());
                         for (const ScenePhaseFragment& p : frags.phases)
                           std::printf("    phase %u %-5s -> %s\n", p.phase,
                                       p.on_begin ? "begin" : "end", p.fragment.function.c_str());
                         break;
                       }
                     });
  std::printf("MQ101 scenes parsed: %d\n", scenes);
  return scenes > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  TestParse();
  TestEmpty();
  TestFragments();

  int rc = 0;
  if (argc >= 2) rc = DumpReal(argv[1]);

  if (g_failures == 0 && rc == 0) {
    std::puts("scene: all checks passed");
    return 0;
  }
  std::printf("scene: %d unit checks FAILED (dump rc=%d)\n", g_failures, rc);
  return 1;
}
