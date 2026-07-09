// vmad_fo4test: deterministic checks that the VMAD parser reads the Fallout 4
// (version 6) layout. FO4 differs from Skyrim (version 5) by an array-of-struct
// property type (17) and an extra property block at the head of every fragment
// tail. Hand-built little-endian VMAD blobs exercise both; if either were
// mis-sized the script names or the quest fragment would not decode. No game
// data needed, so it runs in the ctest gate. Skyrim v5 is covered by the tests
// that parse shipped Skyrim plugins.

#include <cstdio>
#include <string>
#include <vector>

#include "bethesda/script_attachment.h"
#include "core/types.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void PutU8(std::vector<rx::u8>& b, rx::u8 v) { b.push_back(v); }
void PutU16(std::vector<rx::u8>& b, rx::u16 v) {
  b.push_back(rx::u8(v));
  b.push_back(rx::u8(v >> 8));
}
void PutU32(std::vector<rx::u8>& b, rx::u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(rx::u8(v >> (8 * i)));
}
void PutStr(std::vector<rx::u8>& b, const char* s) {  // u16 len + bytes, no terminator
  rx::u16 n = 0;
  while (s[n]) ++n;
  PutU16(b, n);
  for (rx::u16 i = 0; i < n; ++i) b.push_back(static_cast<rx::u8>(s[i]));
}

// A v6 script section with one script carrying one type-17 (array-of-struct)
// property, the FO4-specific type the parser must skip to stay aligned.
void TestScriptSectionType17() {
  std::puts("vmad v6 script section (type 17):");
  std::vector<rx::u8> b;
  PutU16(b, 6);  // version (Fallout 4)
  PutU16(b, 2);  // object format
  PutU16(b, 1);  // script count
  PutStr(b, "QuestScript");
  PutU8(b, 1);   // status (version >= 4)
  PutU16(b, 1);  // property count
  PutStr(b, "structArray");
  PutU8(b, 17);  // type: array of struct
  PutU8(b, 1);   // status
  // type-17 value: element count, each a member count, each name/type/status/value.
  PutU32(b, 1);  // one element
  PutU32(b, 1);  // one member
  PutStr(b, "x");
  PutU8(b, 3);   // member type int
  PutU8(b, 1);   // member status
  PutU32(b, 42);  // member int value

  rx::bethesda::ScriptAttachment att;
  const bool ok = rx::bethesda::ParseScriptAttachment(rx::ByteSpan(b.data(), b.size()), &att);
  Check("parses", ok);
  Check("version 6", att.version == 6);
  Check("one script", att.scripts.size() == 1);
  Check("script name (type-17 value skipped cleanly)",
        att.scripts.size() == 1 && att.scripts[0].name == "QuestScript");
}

// A v6 quest fragment tail: after the shared file name FO4 inserts a property
// block (unknown byte + property list) that v5 lacks. If it is not skipped, the
// fragment entry that follows reads the wrong bytes.
void TestQuestFragmentHeader() {
  std::puts("vmad v6 quest fragment header:");
  std::vector<rx::u8> b;
  PutU16(b, 6);  // version
  PutU16(b, 2);  // object format
  PutU16(b, 1);  // script count
  PutStr(b, "QF_Quest");
  PutU8(b, 1);   // status
  PutU16(b, 0);  // no properties

  // Quest fragment section.
  PutU8(b, 0);   // flags/version
  PutU16(b, 1);  // fragment count
  PutStr(b, "QF_Quest_01000800");  // shared file name
  // FO4 fragment header: unknown byte + property count + properties.
  PutU8(b, 0);   // unknown
  PutU16(b, 0);  // zero fragment-bound properties
  // One fragment entry.
  PutU16(b, 25);  // stage
  PutU16(b, 0);   // unknown
  PutU32(b, 0);   // log index
  PutU8(b, 1);    // per-fragment flags
  PutStr(b, "Frag:25");
  PutStr(b, "Fragment_Stage_0025_Item_00");

  rx::bethesda::ScriptAttachment att;
  std::vector<rx::bethesda::QuestStageFragment> frags;
  const bool ok =
      rx::bethesda::ParseQuestFragments(rx::ByteSpan(b.data(), b.size()), &att, &frags);
  Check("parses", ok);
  Check("one fragment", frags.size() == 1);
  if (frags.size() == 1) {
    Check("fragment stage (FO4 header skipped)", frags[0].stage == 25);
    Check("fragment function", frags[0].function == "Fragment_Stage_0025_Item_00");
  }
}

}  // namespace

int main() {
  TestScriptSectionType17();
  TestQuestFragmentHeader();
  if (g_failures == 0) {
    std::puts("vmad_fo4: all checks passed");
    return 0;
  }
  std::printf("vmad_fo4: %d checks FAILED\n", g_failures);
  return 1;
}
