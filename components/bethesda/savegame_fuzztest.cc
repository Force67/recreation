// savegame_fuzztest: feeds malformed savegames to all three layers of the
// reader and asserts they reject rather than crash.
//
// A .ess is the only file this engine parses that the player chose. It arrives
// off the internet as often as out of the player's own Documents folder, and
// every count, offset and length inside it is a number the file gets to pick.
// Layer 1 walks the container, layer 2 decodes change form payloads and the
// Papyrus heap, layer 3 maps ids and pushes at a sink; all three run before the
// player sees a single frame.
//
// The rule under test is narrow and total: for ANY byte string, the reader
// returns true or false. It never reads out of bounds, never allocates on a
// number it has not bounds-checked, never recurses on file-controlled depth,
// and always terminates. Whether the values it produces are meaningful is the
// other tests' business; this one only cares that a hostile file cannot take
// the process with it.
//
// Deterministic: a fixed seed and a fixed schedule, so it runs in the ctest
// gate and a failure reproduces. Bare it catches hard faults and hangs; built
// with RECREATION_SANITIZE it also catches the silent out-of-bounds reads and
// the undefined casts, which is how it is worth running in CI.
//
// This exists because three real memory-safety bugs shipped in this reader and
// were found by hand: a self-referential push_back that read freed heap, an
// unbounded parent-chain recursion, and a per-record inflate bound with no
// running total. All three are the kind a mutation sweep finds in seconds.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstring>

#include "components/bethesda/savegame.h"
#include "components/bethesda/savegame_apply.h"
#include "components/bethesda/savegame_changeform.h"
#include "components/bethesda/savegame_fixture.h"
#include "components/bethesda/savegame_papyrus.h"
#include "core/log.h"
#include "core/types.h"

using namespace rx;
// rx::u64/i64 (long) and base/arch.h's (long long) are different types sharing
// a global name, so the 64-bit spellings below are qualified; the other scalars
// agree between the two and need no help.
using rx::bethesda::SaveFile;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

// xorshift64: same corpus every run, on every machine.
struct Rng {
  rx::u64 state;
  explicit Rng(rx::u64 seed) : state(seed ? seed : 0x9e3779b97f4a7c15ull) {}
  rx::u64 Next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }
  u32 Below(u32 n) { return n == 0 ? 0 : static_cast<u32>(Next() % n); }
  u8 Byte() { return static_cast<u8>(Next()); }
};

// Reading is the whole point: a parse that returns false has done its job, and
// one that returns true is equally fine. Only a crash, a hang or a sanitizer
// report fails, and those do not come back from the call at all. Counting the
// outcomes keeps the compiler from eliding the work and gives the run a shape
// worth printing.
struct Tally {
  long attempts = 0;
  long accepted = 0;
  void Feed(ByteSpan bytes) {
    SaveFile save;
    ++attempts;
    if (rx::bethesda::ReadSaveFile(bytes, save))
      ++accepted;
    // DetectSaveFormat runs on its own: a caller sniffs the format before
    // committing to a full read, so it sees byte strings ReadSaveFile never
    // does, including ones far too short to be a save.
    rx::bethesda::DetectSaveFormat(bytes);
  }
};

// --- layer 1: the container ------------------------------------------------

// Every prefix of a good save. A truncated file is the single most common way
// a real save arrives broken (a full disk, a killed process, a bad download),
// and every one of these lengths cuts some length field off mid-read.
void TruncationSweep(Tally& t, const base::Vector<u8>& good, const char* what) {
  for (size_t len = 0; len <= good.size(); ++len)
    t.Feed(ByteSpan(good.data(), len));
  std::printf("  %s: %llu truncations\n", what,
              static_cast<unsigned long long>(good.size() + 1));
}

// Single-byte corruption at every offset, several bit patterns each. This is
// what reaches the length and count fields: flipping the top bit of a u32 count
// turns 3 into 2147483651, which is exactly the value a bounds check has to
// survive.
void CorruptionSweep(Tally& t, const base::Vector<u8>& good, const char* what) {
  base::Vector<u8> buf = good;
  const u8 masks[] = {0xFF, 0x01, 0x80, 0x7F};
  long cases = 0;
  for (size_t off = 0; off < buf.size(); ++off) {
    const u8 original = buf[off];
    for (u8 mask : masks) {
      buf[off] = static_cast<u8>(original ^ mask);
      t.Feed(ByteSpan(buf.data(), buf.size()));
      ++cases;
    }
    buf[off] = original;
  }
  std::printf("  %s: %ld single-byte corruptions\n", what, cases);
}

// Multi-byte mutation: several bytes rewritten at once, which reaches states a
// single flip cannot (a count and its matching offset both made hostile, so the
// two do not contradict each other and a "these disagree" check passes).
void MutationSweep(Tally& t, const base::Vector<u8>& good, Rng& rng, const char* what) {
  constexpr int kRounds = 3000;
  base::Vector<u8> buf;
  for (int round = 0; round < kRounds; ++round) {
    buf = good;
    const u32 edits = 1 + rng.Below(12);
    for (u32 i = 0; i < edits; ++i) {
      const size_t at = rng.Below(static_cast<u32>(buf.size()));
      switch (rng.Below(4)) {
        case 0:
          buf[at] = rng.Byte();
          break;
        case 1:
          buf[at] = 0xff;  // saturate a length byte
          break;
        case 2:
          buf[at] = 0x00;
          break;
        default:
          buf[at] = static_cast<u8>(buf[at] ^ 0x80);
          break;
      }
    }
    // Sometimes cut it short as well, so a mutated count meets a short buffer.
    const size_t len = rng.Below(8) == 0 ? rng.Below(static_cast<u32>(buf.size()) + 1) : buf.size();
    t.Feed(ByteSpan(buf.data(), len));
  }
  std::printf("  %s: %d multi-byte mutations\n", what, kRounds);
}

// Files that were never a save: pure noise, and noise behind a valid magic so
// the reader commits to a format before finding the body is gibberish.
void GarbageSweep(Tally& t, Rng& rng) {
  constexpr int kRounds = 4000;
  base::Vector<u8> buf;
  for (int round = 0; round < kRounds; ++round) {
    buf.clear();
    const u32 n = rng.Below(600);
    if (round % 3 == 0) {
      const char* magic = (round % 6 == 0) ? "TESV_SAVEGAME" : "FO4_SAVEGAME";
      const size_t magic_len = std::strlen(magic);
      for (size_t i = 0; i < magic_len; ++i)
        buf.push_back(static_cast<u8>(magic[i]));
    }
    for (u32 i = 0; i < n; ++i)
      buf.push_back(rng.Byte());
    t.Feed(ByteSpan(buf.data(), buf.size()));
  }
  std::printf("  garbage: %d random files\n", kRounds);
}

// --- layer 2: the Papyrus heap ---------------------------------------------

// The heap table is its own parser: a string pool, script definitions with a
// parent chain, and six figures of instances naming them. Both of the memory
// bugs found by hand lived here, in the flattener that walks that chain, so the
// cases have to reach it. Mutating a valid table is what gets them there;
// assembling bytes blind is rejected at the header and proves nothing.
void PapyrusHeapSweep(Rng& rng) {
  base::Vector<u32> form_ids;
  for (u32 id : rx::bethesda::SyntheticSkyrimFormIds())
    form_ids.push_back(id);

  const base::Vector<u8> good = rx::bethesda::BuildSyntheticPapyrusHeap();
  {
    rx::bethesda::PapyrusHeap heap;
    Check("the synthetic papyrus heap parses",
          rx::bethesda::ReadPapyrusHeap(ByteSpan(good.data(), good.size()), form_ids, &heap));
  }

  long accepted = 0, attempts = 0;
  base::Vector<u8> buf;

  // Every prefix: a table cut off mid-string or mid-instance.
  for (size_t len = 0; len <= good.size(); ++len) {
    rx::bethesda::PapyrusHeap heap;
    ++attempts;
    if (rx::bethesda::ReadPapyrusHeap(ByteSpan(good.data(), len), form_ids, &heap))
      ++accepted;
  }

  // Single-byte corruption everywhere. The string and member counts live in
  // here, and those are what the flattener sizes its work from.
  buf = good;
  const u8 masks[] = {0xFF, 0x01, 0x80};
  for (size_t off = 0; off < buf.size(); ++off) {
    const u8 original = buf[off];
    for (u8 mask : masks) {
      buf[off] = static_cast<u8>(original ^ mask);
      rx::bethesda::PapyrusHeap heap;
      ++attempts;
      if (rx::bethesda::ReadPapyrusHeap(ByteSpan(buf.data(), buf.size()), form_ids, &heap))
        ++accepted;
    }
    buf[off] = original;
  }

  // Multi-byte mutation, including a parent index pointed back into the table
  // at random, which is how a chain becomes long or cyclic.
  constexpr int kRounds = 6000;
  for (int round = 0; round < kRounds; ++round) {
    buf = good;
    const u32 edits = 1 + rng.Below(10);
    for (u32 i = 0; i < edits; ++i)
      buf[rng.Below(static_cast<u32>(buf.size()))] = rng.Byte();
    const size_t len = rng.Below(8) == 0 ? rng.Below(static_cast<u32>(buf.size()) + 1) : buf.size();
    rx::bethesda::PapyrusHeap heap;
    ++attempts;
    if (rx::bethesda::ReadPapyrusHeap(ByteSpan(buf.data(), len), form_ids, &heap))
      ++accepted;
  }

  std::printf("  papyrus heap: %ld tables, %ld accepted\n", attempts, accepted);
  Check("a mutated papyrus heap is sometimes still readable", accepted > 0);
}

// The change form decoders each walk a payload whose length the file chose.
// They are handed bytes directly, so they are the shortest path from a hostile
// file to a parser, and every one of them gets the same treatment.
void ChangeFormDecoderSweep(Rng& rng) {
  constexpr int kRounds = 4000;
  long decoded = 0;
  for (int round = 0; round < kRounds; ++round) {
    rx::bethesda::ChangeForm form;
    form.form_id = rng.Next() & 0xffffffffu;
    form.flags = static_cast<u32>(rng.Next());
    form.version = static_cast<u8>(rng.Below(80));
    const u32 n = rng.Below(400);
    form.data.clear();
    for (u32 i = 0; i < n; ++i)
      form.data.push_back(rng.Byte());

    // Every decoder, on the same payload: which one a form goes to is decided
    // by a type byte the file also chose, so none of them may assume shape.
    rx::bethesda::ReferenceChange ref;
    if (rx::bethesda::DecodeReference(form, ref))
      ++decoded;
    rx::bethesda::QuestChange quest;
    rx::bethesda::DecodeQuest(form, quest);
    rx::bethesda::ActorBaseChange actor;
    rx::bethesda::DecodeActorBase(form, actor);
    rx::bethesda::FactionChange faction;
    rx::bethesda::DecodeFaction(form, faction);
    rx::bethesda::DialogueInfoChange info;
    rx::bethesda::DecodeDialogueInfo(form, info);
    rx::bethesda::WordOfPowerChange word;
    rx::bethesda::DecodeWordOfPower(form, word);
    rx::bethesda::CellChange cell;
    rx::bethesda::DecodeCell(form, cell);
    rx::bethesda::LocationChange location;
    rx::bethesda::DecodeLocation(form, location);
  }
  std::printf("  change form decoders: %d payloads through 8 decoders\n", kRounds);
}

// --- layer 3: applying onto a world ----------------------------------------

// Counts what it is told and holds nothing, so ApplySave can be driven over
// mutated saves without a world behind it. The point is the walk, not the
// writes: layer 3 dereferences what layer 2 produced, and a decoder that
// returned a nonsense count reaches the engine through here.
class CountingSink : public rx::bethesda::SaveSink {
 public:
  void SetGlobal(rx::bethesda::GlobalFormId, f32) override { ++globals; }
  void SetQuestStageDone(rx::bethesda::GlobalFormId, i32) override { ++stages; }
  void SetReferenceEnabled(rx::bethesda::GlobalFormId, bool) override { ++refs; }
  void SetReferenceDeleted(rx::bethesda::GlobalFormId) override { ++refs; }
  void SetContainerEmptied(rx::bethesda::GlobalFormId) override { ++refs; }
  long globals = 0, stages = 0, refs = 0;
};

void ApplySweep(Rng& rng, const base::Vector<u8>& good) {
  constexpr int kRounds = 2000;
  long applied = 0;
  base::Vector<u8> buf;
  for (int round = 0; round < kRounds; ++round) {
    buf = good;
    const u32 edits = 1 + rng.Below(10);
    for (u32 i = 0; i < edits; ++i)
      buf[rng.Below(static_cast<u32>(buf.size()))] = rng.Byte();

    SaveFile save;
    if (!rx::bethesda::ReadSaveFile(ByteSpan(buf.data(), buf.size()), save))
      continue;
    // A remap that resolves nothing is the interesting case, not a degenerate
    // one: it is what a save written with mods the player has since removed
    // produces, and every id then takes the refusal path.
    rx::bethesda::FormRemap remap;
    remap.Build(save, [](const base::String&) -> u16 { return 0xffff; });
    CountingSink sink;
    rx::bethesda::SaveApplyStats stats;
    rx::bethesda::ApplySave(save, remap, sink, &stats);
    ++applied;
  }
  std::printf("  apply: %ld mutated saves walked end to end\n", applied);
}

}  // namespace

int main() {
  std::printf("savegame_fuzztest\n");
  // A rejected file logs why it was rejected, which is right in the engine and
  // useless here: rejecting is the expected outcome tens of thousands of times
  // over, and the warnings bury the result.
  rx::SetLogLevel(rx::LogLevel::kError);

  const base::Vector<u8> skyrim = rx::bethesda::BuildSyntheticSkyrimLeSave();
  const base::Vector<u8> fallout = rx::bethesda::BuildSyntheticFallout4Save();

  // The seeds have to be readable, or every sweep below is only testing the
  // reject path and would pass just as well against a parser that refuses
  // everything.
  {
    SaveFile save;
    Check("the synthetic skyrim save reads",
          rx::bethesda::ReadSaveFile(ByteSpan(skyrim.data(), skyrim.size()), save));
    Check("it is detected as skyrim le",
          rx::bethesda::DetectSaveFormat(ByteSpan(skyrim.data(), skyrim.size())) ==
              rx::bethesda::SaveFormat::kSkyrimLe);
  }
  {
    SaveFile save;
    Check("the synthetic fallout 4 save reads",
          rx::bethesda::ReadSaveFile(ByteSpan(fallout.data(), fallout.size()), save));
    Check("it is detected as fallout 4",
          rx::bethesda::DetectSaveFormat(ByteSpan(fallout.data(), fallout.size())) ==
              rx::bethesda::SaveFormat::kFallout4);
  }

  Rng rng(0x5a3ec0de5a3ec0deull);
  Tally tally;

  TruncationSweep(tally, skyrim, "skyrim");
  TruncationSweep(tally, fallout, "fallout 4");
  CorruptionSweep(tally, skyrim, "skyrim");
  CorruptionSweep(tally, fallout, "fallout 4");
  MutationSweep(tally, skyrim, rng, "skyrim");
  MutationSweep(tally, fallout, rng, "fallout 4");
  GarbageSweep(tally, rng);
  PapyrusHeapSweep(rng);
  ChangeFormDecoderSweep(rng);
  ApplySweep(rng, skyrim);

  std::printf("  container: %ld files fed, %ld accepted\n", tally.attempts, tally.accepted);
  // Reaching here at all is the result: every call above returned. A reader
  // that accepted nothing would still be wrong, so hold the seeds to account.
  Check("the sweeps ran", tally.attempts > 10000);
  Check("well-formed input is still accepted", tally.accepted > 0);

  if (g_failures != 0) {
    std::printf("FAILURES\n");
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
