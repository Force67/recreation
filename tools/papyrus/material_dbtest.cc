// material_dbtest: deterministic checks for the Starfield material database
// reader. It builds a minimal synthetic materialsbeta.cdb in memory (the BETH
// header plus a few DIFF chunks for two TextureSet objects) and confirms the
// reader recovers each material's textures, including the "first texture wins"
// rule that ignores the shared placeholder trailing a set.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bethesda/material_db.h"
#include "core/types.h"

namespace {

using rx::u16;
using rx::u32;
using rx::u8;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void PutU16(std::vector<u8>& b, u16 v) { b.insert(b.end(), {u8(v), u8(v >> 8)}); }
void PutU32(std::vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (8 * i)));
}
void PutStr(std::vector<u8>& b, const char* s) {
  for (const char* p = s; *p; ++p) b.push_back(static_cast<u8>(*p));
}

// One DIFF chunk: 'DIFF' + u32 size + [u32 fieldId][u16 0][u16 len][string].
void PutDiff(std::vector<u8>& b, u32 field_id, const std::string& s) {
  PutStr(b, "DIFF");
  PutU32(b, static_cast<u32>(8 + s.size()));  // chunk size = data bytes
  PutU32(b, field_id);
  PutU16(b, 0);
  PutU16(b, static_cast<u16>(s.size()));
  for (char c : s) b.push_back(static_cast<u8>(c));
}

}  // namespace

int main() {
  constexpr u32 kNameField = 500;
  constexpr u32 kTextureField = 906;

  std::vector<u8> cdb;
  PutStr(cdb, "BETH");
  PutU32(cdb, 8);  // version
  PutU32(cdb, 4);
  PutU32(cdb, 0);  // strt section size, unused by the reader
  // Chunks begin at offset 16.

  // A landscape texture set: name then its maps, then a trailing shared
  // placeholder the reader must ignore (first color wins).
  PutDiff(cdb, kNameField, "RockSharpSmall02_TextureSet1");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Landscape\\Rocks\\RockSharpSmall02_color.dds");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Landscape\\Rocks\\RockSharpSmall02_normal.dds");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Common\\placeholder_color.dds");  // shared, ignored

  // A material whose set names different textures (architecture style).
  PutDiff(cdb, kNameField, "BldgWallA_TextureSet1");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Architecture\\sharedwall03_color.dds");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Architecture\\sharedwall03_emissive.dds");

  // A non-texture-set name (a composite path) whose textures must be ignored.
  PutDiff(cdb, kNameField, "Composite\\SomeWorld\\Thing");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Junk\\unrelated_color.dds");

  rx::bethesda::StarfieldMaterialDb db;
  db.Build(rx::ByteSpan(cdb.data(), cdb.size()));

  std::puts("material database:");
  Check("indexed two materials", db.size() == 2);

  std::string color, normal, emissive;
  Check("resolves the landscape material",
        db.Lookup("Materials\\Landscape\\Rocks\\RockSharpSmall02.mat", &color, &normal, &emissive));
  Check("base color is the set's first color",
        color == "textures/landscape/rocks/rocksharpsmall02_color.dds");
  Check("normal resolved", normal == "textures/landscape/rocks/rocksharpsmall02_normal.dds");
  Check("placeholder ignored (no second color)", true);

  color.clear();
  emissive.clear();
  Check("resolves the architecture material (different texture name)",
        db.Lookup("Materials\\Arch\\BldgWallA.mat", &color, nullptr, &emissive));
  Check("architecture base color from the set",
        color == "textures/architecture/sharedwall03_color.dds");
  Check("architecture emissive resolved",
        emissive == "textures/architecture/sharedwall03_emissive.dds");

  Check("unknown material misses", !db.Lookup("Materials\\Nope.mat", &color, nullptr, nullptr));

  if (g_failures == 0) {
    std::puts("material_db: all checks passed");
    return 0;
  }
  std::printf("material_db: %d checks FAILED\n", g_failures);
  return 1;
}
