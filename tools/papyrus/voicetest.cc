// voicetest: the voice-asset naming a spoken line resolves through, which is what
// gives cutscene lines their real length. Checks the path format against real
// Skyrim file names and the candidate order. Pure, no game data.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "dialogue/voice.h"

using namespace rx;
using namespace rx::dialogue;

namespace {

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void TestPaths() {
  std::puts("voice (paths):");
  // Both spellings exist in Skyrim - Voices_en0.bsa: scene lines file with an
  // empty topic segment, ordinary topics keep the DIAL editor id.
  Check("a scene line with no topic id",
        VoiceFilePath("Skyrim.esm", "MaleNord", "MQ101", "", 0x3374b, 1) ==
            "sound/voice/skyrim.esm/malenord/mq101__0003374b_1.fuz");
  Check("a topic line keeps the topic id",
        VoiceFilePath("Skyrim.esm", "MaleEvenTonedAccented", "MQ101", "MQ101SoldierBlocking",
                      0x648fc, 1) ==
            "sound/voice/skyrim.esm/maleeventonedaccented/"
            "mq101_mq101soldierblocking_000648fc_1.fuz");
  Check("the response index is part of the name",
        VoiceFilePath("Dawnguard.esm", "FemaleNord", "DLC1VQ01", "", 0x1234, 3) ==
            "sound/voice/dawnguard.esm/femalenord/dlc1vq01__00001234_3.fuz");
}

void TestCandidates() {
  std::puts("voice (candidates):");
  const std::vector<std::string> plugins = {"Skyrim.esm", "Update.esm"};
  const std::vector<std::string> got =
      VoiceFileCandidates(plugins, "MaleNord", "MQ101", "SomeTopic", 0x3374b, 2);
  Check("the plugin the line came from is probed first",
        !got.empty() && got[0].find("skyrim.esm") != std::string::npos);
  Check("the empty-topic spelling is probed too",
        std::find(got.begin(), got.end(),
                  std::string("sound/voice/skyrim.esm/malenord/mq101__0003374b_2.fuz")) !=
            got.end());
  Check("the first response is probed as a fallback",
        std::find(got.begin(), got.end(),
                  std::string("sound/voice/skyrim.esm/malenord/mq101_sometopic_0003374b_1.fuz")) !=
            got.end());
  Check("every plugin is covered",
        std::find_if(got.begin(), got.end(), [](const std::string& p) {
          return p.find("update.esm") != std::string::npos;
        }) != got.end());
  Check("a voiceless speaker yields nothing to probe",
        VoiceFileCandidates(plugins, "", "MQ101", "", 1, 1).empty());
}

// Builds a .fuz the way Skyrim ships one: lipsync block, then an xWMA RIFF whose
// dpds table ends at the clip's total decoded byte count. The numbers are the ones
// out of sound/voice/skyrim.esm/femalecommoner/mq101__00108ceb_1.fuz (mono 16-bit
// 44100 Hz, 0x29000 = 167936 decoded bytes = 83968 frames = 1.904 s, which is what
// ffprobe reports for that file).
std::vector<rx::u8> MakeFuz() {
  std::vector<rx::u8> out;
  auto put = [&](const void* data, size_t n) {
    const rx::u8* p = static_cast<const rx::u8*>(data);
    out.insert(out.end(), p, p + n);
  };
  auto put_u32 = [&](rx::u32 v) { put(&v, 4); };
  auto put_u16 = [&](rx::u16 v) { put(&v, 2); };
  put("FUZE", 4);
  put_u32(1);   // version
  put_u32(4);   // lipsync block size
  put_u32(0);   // the block itself
  put("RIFF", 4);
  put_u32(0);  // riff size, unused by the reader
  put("XWMA", 4);
  put("fmt ", 4);
  put_u32(18);
  put_u16(0x0161);  // WMAudio2
  put_u16(1);       // channels
  put_u32(44100);   // sample rate
  put_u32(4000);    // byte rate
  put_u16(1487);    // block align
  put_u16(16);      // bits
  put_u16(0);       // cbSize
  put("dpds", 4);
  put_u32(8);
  put_u32(0x14000);
  put_u32(0x29000);  // total decoded bytes
  put("data", 4);
  put_u32(13383);
  return out;
}

void TestClipSeconds() {
  std::puts("voice (clip length from the header):");
  const std::vector<rx::u8> fuz = MakeFuz();
  const rx::f32 seconds = ClipSeconds(rx::ByteSpan{fuz.data(), fuz.size()});
  Check("a fuz reports its xWMA decoded length", seconds > 1.900f && seconds < 1.908f);
  const std::vector<rx::u8> junk = {'n', 'o', 'p', 'e'};
  Check("junk reports nothing", ClipSeconds(rx::ByteSpan{junk.data(), junk.size()}) == 0.0f);
  Check("an empty buffer reports nothing", ClipSeconds(rx::ByteSpan{}) == 0.0f);
}

void TestEstimate() {
  std::puts("voice (fallback length):");
  const f32 short_line = EstimateLineSeconds("Yes.");
  const f32 long_line = EstimateLineSeconds(
      "Look at him, General Tullius the Military Governor. And it looks like the Thalmor are with "
      "him. Damn elves.");
  Check("a short line still holds the beat", short_line >= 1.5f);
  Check("a longer line takes longer", long_line > short_line);
  Check("but never parks the scene", long_line <= 9.0f);
}

}  // namespace

int main() {
  TestPaths();
  TestCandidates();
  TestClipSeconds();
  TestEstimate();
  if (g_failures) {
    std::printf("voice: %d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::puts("voice: all checks passed");
  return 0;
}
