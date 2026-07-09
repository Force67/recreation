// dds_srgb_fo4test: pins the sRGB-vs-linear decision ConvertDds makes for
// Fallout 4 DDS textures. Bethesda authors color maps as the plain UNORM DXGI
// formats (BC1_UNORM, BC3_UNORM, ...), never the _SRGB variants, so a UNORM
// color format must defer to the path suffix: a "_d"/base map is sRGB albedo,
// a "_n" map is a linear normal map. A regression here washes out (or darkens)
// every FO4 surface. No game data needed; the blobs are hand-built DX10 DDS.

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include "asset/asset_id.h"
#include "bethesda/converters.h"
#include "core/types.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Builds a minimal DX10 DDS holding one 4x4 BC block of `dxgi` format. The
// header layout matches ConvertDds: magic, 124-byte header (h@12, w@16,
// mips@28, pf flags@80, fourcc@84, caps2@112), then the 20-byte DX10 tail
// (dxgi@128) and the block payload at offset 148.
std::vector<rx::u8> MakeDx10Dds(rx::u32 dxgi, rx::u32 block_bytes) {
  std::vector<rx::u8> b(148, 0);
  auto put = [&](size_t off, rx::u32 v) { std::memcpy(b.data() + off, &v, 4); };
  b[0] = 'D'; b[1] = 'D'; b[2] = 'S'; b[3] = ' ';
  put(12, 4);     // height
  put(16, 4);     // width
  put(28, 1);     // mip count
  put(80, 0x4);   // pixelformat flags: DDPF_FOURCC
  b[84] = 'D'; b[85] = 'X'; b[86] = '1'; b[87] = '0';  // fourcc DX10
  put(128, dxgi);                                      // dxgi format
  b.resize(148 + block_bytes, 0);                      // one block of payload
  return b;
}

bool DecodeSrgb(rx::u32 dxgi, rx::u32 block_bytes, std::string_view path) {
  auto blob = MakeDx10Dds(dxgi, block_bytes);
  auto tex = rx::bethesda::ConvertDds(rx::ByteSpan(blob.data(), blob.size()),
                                       rx::asset::MakeAssetId(path), path);
  return tex && tex->is_srgb;
}

bool Decodes(rx::u32 dxgi, rx::u32 block_bytes, std::string_view path) {
  auto blob = MakeDx10Dds(dxgi, block_bytes);
  return rx::bethesda::ConvertDds(rx::ByteSpan(blob.data(), blob.size()),
                                   rx::asset::MakeAssetId(path), path) != nullptr;
}

constexpr rx::u32 kBc1Block = 8;
constexpr rx::u32 kBc3Block = 16;
constexpr rx::u32 kBc5Block = 16;
constexpr rx::u32 kBc7Block = 16;
constexpr rx::u32 kRgba4x4 = 4 * 4 * 4;  // uncompressed 4x4 BGRA/RGBA payload

void TestColorMapsAreSrgb() {
  std::puts("UNORM color maps with a diffuse path decode as sRGB:");
  // 71=BC1_UNORM, 77=BC3_UNORM, 98=BC7_UNORM, 87=B8G8R8A8_UNORM.
  Check("bc1 _d diffuse -> sRGB", DecodeSrgb(71, kBc1Block, "textures/wall_d.dds"));
  Check("bc3 _d diffuse -> sRGB", DecodeSrgb(77, kBc3Block, "textures/wall_d.dds"));
  Check("bc7 base name -> sRGB", DecodeSrgb(98, kBc7Block, "textures/armor.dds"));
  Check("bgra8 _d diffuse -> sRGB", DecodeSrgb(87, kRgba4x4, "textures/sign_d.dds"));
}

void TestNormalMapsStayLinear() {
  std::puts("UNORM normal/data maps stay linear:");
  Check("bc1 _n normal -> linear", !DecodeSrgb(71, kBc1Block, "textures/wall_n.dds"));
  Check("bc3 _n normal -> linear", !DecodeSrgb(77, kBc3Block, "textures/wall_n.dds"));
  Check("bc7 _msn normal -> linear", !DecodeSrgb(98, kBc7Block, "textures/face_msn.dds"));
  // BC5 is two-channel data (normals); never color regardless of suffix.
  Check("bc5 -> linear", !DecodeSrgb(83, kBc5Block, "textures/wall_d.dds"));
}

void TestExplicitSrgbFormat() {
  std::puts("explicit _SRGB DXGI formats are sRGB even on a data-looking path:");
  // 72=BC1_SRGB, 78=BC3_SRGB, 99=BC7_SRGB.
  Check("bc1 srgb format -> sRGB", DecodeSrgb(72, kBc1Block, "textures/wall_n.dds"));
  Check("bc7 srgb format -> sRGB", DecodeSrgb(99, kBc7Block, "textures/wall_n.dds"));
}

void TestFormatsDecode() {
  std::puts("the FO4 format zoo all decodes (no nullptr):");
  Check("bc1 unorm", Decodes(71, kBc1Block, "textures/a_d.dds"));
  Check("bc3 unorm", Decodes(77, kBc3Block, "textures/a_d.dds"));
  Check("bc5", Decodes(83, kBc5Block, "textures/a_n.dds"));
  Check("bc7 unorm", Decodes(98, kBc7Block, "textures/a_d.dds"));
  Check("bgra8 unorm", Decodes(87, kRgba4x4, "textures/a_d.dds"));
}

}  // namespace

int main() {
  TestColorMapsAreSrgb();
  TestNormalMapsStayLinear();
  TestExplicitSrgbFormat();
  TestFormatsDecode();
  if (g_failures == 0) {
    std::puts("dds_srgb_fo4: all checks passed");
    return 0;
  }
  std::printf("dds_srgb_fo4: %d checks FAILED\n", g_failures);
  return 1;
}
