// bgsm_fo4test: deterministic checks for the Fallout 4 BGSM/BGEM material
// parser. The texture block starts at a fixed offset and BGSM smoothness sits a
// fixed distance past the ten slots; hand-built v2 materials pin those offsets
// (a regression there would mis-bind every FO4 texture). No game data needed.

#include <cstdio>
#include <cstring>
#include <vector>

#include "bethesda/converters.h"
#include "core/types.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void PutU32(std::vector<rx::u8>& b, rx::u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(rx::u8(v >> (8 * i)));
}
void PutF32(std::vector<rx::u8>& b, float f) {
  rx::u32 v;
  std::memcpy(&v, &f, 4);
  PutU32(b, v);
}
// A BGSM/BGEM texture slot: u32 length (including the null) + the bytes.
void PutSlot(std::vector<rx::u8>& b, const char* s) {
  if (!s) {
    PutU32(b, 0);
    return;
  }
  rx::u32 n = 0;
  while (s[n]) ++n;
  PutU32(b, n + 1);
  for (rx::u32 i = 0; i < n; ++i) b.push_back(static_cast<rx::u8>(s[i]));
  b.push_back(0);  // null terminator counted in the length
}

constexpr rx::u32 kTextureOffset = 63;  // fixed v2 pre-texture header size
constexpr rx::u32 kSmoothnessGap = 28;

void TestBgsm() {
  std::puts("bgsm v2 textures + smoothness:");
  std::vector<rx::u8> b;
  for (char c : {'B', 'G', 'S', 'M'}) b.push_back(static_cast<rx::u8>(c));
  PutU32(b, 2);  // version
  while (b.size() < kTextureOffset) b.push_back(0);  // pre-texture header padding

  PutSlot(b, "Test\\base_d.dds");    // slot 0: diffuse
  PutSlot(b, "Test\\base_n.dds");    // slot 1: normal
  for (int i = 2; i < 10; ++i) PutSlot(b, nullptr);  // slots 2..9 empty
  for (rx::u32 i = 0; i < kSmoothnessGap; ++i) b.push_back(0);  // pre-smoothness block
  PutF32(b, 0.25f);  // smoothness -> roughness 0.75

  rx::bethesda::BgsmMaterial m;
  const bool ok = rx::bethesda::ParseBgsm(rx::ByteSpan(b.data(), b.size()), &m);
  Check("parses", ok);
  Check("diffuse normalized to textures/", m.diffuse == "textures/test/base_d.dds");
  Check("normal normalized to textures/", m.normal == "textures/test/base_n.dds");
  Check("roughness = 1 - smoothness", m.roughness > 0.74f && m.roughness < 0.76f);
}

void TestBgem() {
  std::puts("bgem v2 (no normal, no smoothness):");
  std::vector<rx::u8> b;
  for (char c : {'B', 'G', 'E', 'M'}) b.push_back(static_cast<rx::u8>(c));
  PutU32(b, 2);
  while (b.size() < kTextureOffset) b.push_back(0);
  PutSlot(b, "Effects\\glow_d.dds");
  PutSlot(b, "Effects\\grayscale.dds");  // BGEM slot 1 is not a normal map

  rx::bethesda::BgsmMaterial m;
  const bool ok = rx::bethesda::ParseBgsm(rx::ByteSpan(b.data(), b.size()), &m);
  Check("parses", ok);
  Check("diffuse set", m.diffuse == "textures/effects/glow_d.dds");
  Check("normal empty (bgem slot 1 ignored)", m.normal.empty());
  Check("roughness unknown for bgem", m.roughness < 0.0f);
}

void TestRejectsOther() {
  std::puts("bgsm rejects non-v2 / non-material:");
  std::vector<rx::u8> v20;
  for (char c : {'B', 'G', 'S', 'M'}) v20.push_back(static_cast<rx::u8>(c));
  PutU32(v20, 20);  // FO76 version, unsupported here
  while (v20.size() < kTextureOffset + 8) v20.push_back(0);
  rx::bethesda::BgsmMaterial m;
  Check("v20 rejected", !rx::bethesda::ParseBgsm(rx::ByteSpan(v20.data(), v20.size()), &m));

  std::vector<rx::u8> junk(80, 0);
  Check("non-material rejected",
        !rx::bethesda::ParseBgsm(rx::ByteSpan(junk.data(), junk.size()), &m));
}

}  // namespace

int main() {
  TestBgsm();
  TestBgem();
  TestRejectsOther();
  if (g_failures == 0) {
    std::puts("bgsm_fo4: all checks passed");
    return 0;
  }
  std::printf("bgsm_fo4: %d checks FAILED\n", g_failures);
  return 1;
}
