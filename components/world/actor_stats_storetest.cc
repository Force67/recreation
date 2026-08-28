// actor_stats_storetest: what a placed actor comes up with. The record answers
// unless a resumed savegame overrode that group, and a level-mult actor is
// resolved against the player before anyone sees it. Synthetic records, so no
// game data is needed.

#include <base/containers/vector.h>
#include <base/memory/move.h>

#include <cstdio>
#include <cstring>

#include "components/world/actor_stats_store.h"

namespace {

using rx::u32;
using rx::u8;
using rx::bethesda::ActorAi;
using rx::bethesda::ActorStats;
using rx::bethesda::GlobalFormId;
using rx::world::ActorStatsStore;

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

constexpr GlobalFormId kAhtar{0, 0x0001325F};

// Backing store for the subrecord spans a synthetic Record points at.
struct Buffers {
  base::Vector<base::Vector<u8>> store;
  rx::ByteSpan Take(base::Vector<u8> bytes) {
    auto& b = store.emplace_back(base::move(bytes));
    return rx::ByteSpan(b.data(), b.size());
  }
};

void Add(rx::bethesda::Record& record, u32 type, rx::ByteSpan data) {
  rx::bethesda::Subrecord sub;
  sub.type = type;
  sub.data = data;
  record.subrecords.push_back(base::move(sub));
}

void PutU16(base::Vector<u8>& b, rx::u16 v) {
  b.push_back(static_cast<u8>(v));
  b.push_back(static_cast<u8>(v >> 8));
}

// Ahtar's ACBS as Skyrim.esm writes it: level-mult set, multiplier 1000
// (1.0x), calc range 6..30.
base::Vector<u8> AhtarAcbs() {
  base::Vector<u8> b;
  const u32 flags = 0x000000B0;  // includes kActorBaseFlagLevelMult (0x80)
  for (int i = 0; i < 4; ++i)
    b.push_back(static_cast<u8>(flags >> (8 * i)));
  PutU16(b, 0);     // magicka offset
  PutU16(b, 0);     // stamina offset
  PutU16(b, 1000);  // level multiplier
  PutU16(b, 6);     // calc min
  PutU16(b, 30);    // calc max
  PutU16(b, 100);   // speed multiplier
  PutU16(b, 0);     // disposition base
  PutU16(b, 0);     // template flags
  PutU16(b, 0);     // health offset
  PutU16(b, 0);     // bleedout override
  return b;
}

// Ahtar's AIDT: unaggressive, brave, energy 50, no crime, mood 7, helps friends.
base::Vector<u8> AhtarAidt() {
  base::Vector<u8> b{0, 3, 50, 3, 7, 2};
  b.resize(20);
  return b;
}

rx::bethesda::Record AhtarRecord(Buffers& buf) {
  rx::bethesda::Record record;
  Add(record, rx::FourCc('A', 'C', 'B', 'S'), buf.Take(AhtarAcbs()));
  Add(record, rx::FourCc('A', 'I', 'D', 'T'), buf.Take(AhtarAidt()));
  return record;
}

void TestRecordAnswers() {
  Buffers buf;
  const rx::bethesda::Record ahtar = AhtarRecord(buf);

  ActorStatsStore store;
  ActorStats fresh = store.For(kAhtar, ahtar);
  Check("an unlevelled player leaves a 1.0x actor at his calc floor", fresh.level == 6);
  Check("the temperament comes off AIDT",
        fresh.has_ai && fresh.ai.aggression == 0 && fresh.ai.confidence == 3 &&
            fresh.ai.energy == 50 && fresh.ai.morality == 3 && fresh.ai.mood == 7 &&
            fresh.ai.assistance == 2);

  store.set_player_level(271);
  Check("a level 271 player pushes him to his ceiling", store.For(kAhtar, ahtar).level == 30);
  store.set_player_level(17);
  Check("inside the range he tracks the player", store.For(kAhtar, ahtar).level == 17);
}

void TestOverrides() {
  Buffers buf;
  const rx::bethesda::Record ahtar = AhtarRecord(buf);
  ActorStatsStore store;

  store.OverrideLevel(kAhtar, 30);
  const ActorStats levelled = store.For(kAhtar, ahtar);
  Check("a savegame level wins over the record", levelled.level == 30);
  Check("its temperament still comes off the record",
        levelled.has_ai && levelled.ai.confidence == 3);

  store.OverrideAi(kAhtar, ActorAi{2, 4, 60, 0, 1, 1});
  const ActorStats both = store.For(kAhtar, ahtar);
  Check("a savegame temperament wins too",
        both.has_ai && both.ai.aggression == 2 && both.ai.confidence == 4 &&
            both.ai.assistance == 1);
  Check("one override entry per actor", store.overrides() == 1);

  const ActorStats other = store.For(GlobalFormId{0, 0x00013269}, ahtar);
  Check("another actor is not touched by it", other.level == 6 && other.ai.aggression == 0);
}

void TestRecordWithoutStats() {
  rx::bethesda::Record empty;
  ActorStatsStore store;
  const ActorStats stats = store.For(kAhtar, empty);
  Check("a record with no ACBS is level 1 with no temperament",
        stats.level == 1 && !stats.has_ai);
}

}  // namespace

int main() {
  std::puts("actor_stats_storetest");
  TestRecordAnswers();
  TestOverrides();
  TestRecordWithoutStats();
  std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
  return g_failures == 0 ? 0 : 1;
}
