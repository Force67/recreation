// shader_packagetest: checks for the shipped shader-library decoders.
// Builds a synthetic .fxp (grouped DXBC records, reflection-free like the real
// ones) and a synthetic .sdp (named D3D9 blobs), so neither needs game data.

#include <base/containers/vector.h>

#include <cstdio>
#include <cstring>

#include "components/bethesda/shader_package.h"
#include "core/types.h"

namespace {

using rx::u32;
using rx::u64;
using rx::u8;
using rx::bethesda::ShaderStage;

int g_failures = 0;
void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

void PutU32(base::Vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i)
    b.push_back(u8(v >> (8 * i)));
}
void PutBytes(base::Vector<u8>& b, const void* p, size_t n) {
  const u8* s = static_cast<const u8*>(p);
  for (size_t i = 0; i < n; ++i)
    b.push_back(s[i]);
}

// A DXBC container holding one SHEX chunk, which is all the parser reads: the
// total size (validated against the record) and the program-type token.
base::Vector<u8> MakeDxbc(u32 program_type, u32 filler_dwords) {
  base::Vector<u8> b;
  PutBytes(b, "DXBC", 4);
  for (int i = 0; i < 16; ++i)
    b.push_back(u8(0xa0 + i));  // digest
  PutU32(b, 1);                 // version
  PutU32(b, 0);                 // total size, patched below
  PutU32(b, 1);                 // chunk count
  PutU32(b, 0x24);              // chunk table: one entry at 0x24
  // SHEX chunk: tag, byte size, version token, dword length, then filler.
  PutBytes(b, "SHEX", 4);
  PutU32(b, 8 + filler_dwords * 4);
  PutU32(b, (program_type << 16) | 0x50);
  PutU32(b, 2 + filler_dwords);
  for (u32 i = 0; i < filler_dwords; ++i)
    PutU32(b, 0x01000000u | i);

  const u32 total = static_cast<u32>(b.size());
  std::memcpy(b.data() + 0x18, &total, 4);
  return b;
}

// One .fxp record: magic, bytecode size, technique id, descriptor, bytecode.
void PutFxpRecord(base::Vector<u8>& b,
                  u32 technique_id,
                  const base::Vector<u8>& descriptor,
                  const base::Vector<u8>& dxbc) {
  PutU32(b, 0x11223344);
  PutU32(b, static_cast<u32>(dxbc.size()));
  PutU32(b, technique_id);
  PutBytes(b, descriptor.data(), descriptor.size());
  PutBytes(b, dxbc.data(), dxbc.size());
}

// 32-byte vertex descriptor: packed vertex layout then the constant offsets.
base::Vector<u8> VertexDescriptor(u64 vertex_desc) {
  base::Vector<u8> d;
  for (int i = 0; i < 8; ++i)
    d.push_back(u8(vertex_desc >> (8 * i)));
  const u8 offsets[24] = {0x00, 0x0c, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x34,
                          0x38, 0x00, 0x04, 0x08, 0x00, 0x04, 0x08, 0x0c,
                          0, 0, 0, 0, 0x04, 0x03, 0x0f, 0x00};
  PutBytes(d, offsets, sizeof(offsets));
  return d;
}

base::Vector<u8> PixelDescriptor(u8 marker) {
  base::Vector<u8> d;
  for (int i = 0; i < 68; ++i)
    d.push_back(0);
  d[67] = marker;
  return d;
}

void PutSdpRecord(base::Vector<u8>& b, const char* name, const base::Vector<u8>& code) {
  const size_t start = b.size();
  for (size_t i = 0; i < 256; ++i)
    b.push_back(0xfd);  // the real files pad the name field with filler
  const size_t len = std::strlen(name);
  std::memcpy(b.data() + start, name, len);
  b[start + len] = 0;
  PutU32(b, static_cast<u32>(code.size()));
  PutBytes(b, code.data(), code.size());
}

base::Vector<u8> MakeD3d9(u32 stage_token, u32 dwords) {
  base::Vector<u8> b;
  PutU32(b, stage_token);
  for (u32 i = 0; i < dwords; ++i)
    PutU32(b, 0x0000ffffu);
  return b;
}

void TestFxp() {
  std::puts("fxp (Skyrim SE / Fallout 4 shadersfx)");

  const u64 kVertexDesc = 0x06c1b00000ff0fffull;
  base::Vector<u8> blob;
  // Group 0: two vertex shaders then two pixel shaders.
  PutU32(blob, 2);
  PutU32(blob, 2);
  PutFxpRecord(blob, 0x00000000, VertexDescriptor(kVertexDesc), MakeDxbc(1, 4));
  PutFxpRecord(blob, 0x00000001, VertexDescriptor(kVertexDesc), MakeDxbc(1, 6));
  PutFxpRecord(blob, 0x00010000, PixelDescriptor(0x5a), MakeDxbc(0, 8));
  PutFxpRecord(blob, 0x00010001, PixelDescriptor(0x6e), MakeDxbc(0, 3));
  // Group 1: a compute group, which carries a single count where the paired
  // groups carry two, and a descriptor of its own size.
  PutU32(blob, 1);
  base::Vector<u8> compute_descriptor;
  for (int i = 0; i < 36; ++i)
    compute_descriptor.push_back(u8(i));
  PutFxpRecord(blob, 0x00000007, compute_descriptor, MakeDxbc(5, 5));

  rx::bethesda::ShaderPackage p =
      rx::bethesda::ParseShaderPackage(rx::ByteSpan(blob.data(), blob.size()));

  Check("parses valid", p.valid);
  Check("five shaders", p.shaders.size() == 5);
  Check("two groups", p.groups.size() == 2);
  if (p.shaders.size() != 5 || p.groups.size() != 2)
    return;

  Check("group 0 counts", p.groups[0].vertex_count == 2 && p.groups[0].pixel_count == 2);
  Check("group 1 is a compute group", p.groups[1].compute_count == 1 &&
                                          p.groups[1].vertex_count == 0 &&
                                          p.groups[1].pixel_count == 0);
  Check("group 1 starts after group 0", p.groups[1].first_shader == 4);
  Check("vertex stage from bytecode", p.shaders[0].stage == ShaderStage::kVertex);
  Check("pixel stage from bytecode", p.shaders[2].stage == ShaderStage::kPixel);
  Check("compute stage", p.shaders[4].stage == ShaderStage::kCompute);
  Check("technique id", p.shaders[2].technique_id == 0x00010000);
  Check("group index recorded", p.shaders[4].group == 1);
  Check("vertex layout decoded", p.shaders[0].vertex_desc == kVertexDesc);
  Check("vertex descriptor is 32 bytes", p.shaders[0].descriptor.size() == 32);
  Check("pixel descriptor is 68 bytes", p.shaders[2].descriptor.size() == 68);
  Check("compute descriptor is 36 bytes", p.shaders[4].descriptor.size() == 36);
  Check("descriptor content preserved", p.shaders[2].descriptor.data()[67] == 0x5a);
  Check("bytecode is the container", p.shaders[0].bytecode.size() >= 0x24 &&
                                        std::memcmp(p.shaders[0].bytecode.data(), "DXBC", 4) == 0);
  // No name survives the strip: .fxp records carry none.
  Check("fxp shaders are unnamed", p.shaders[0].name.empty());

  // A record whose declared size disagrees with the container is rejected, and
  // the shaders read before it are still reported.
  base::Vector<u8> corrupt = blob;
  corrupt[12] = 0xff;  // bytecode size of the first record
  rx::bethesda::ShaderPackage bad =
      rx::bethesda::ParseShaderPackage(rx::ByteSpan(corrupt.data(), corrupt.size()));
  Check("size mismatch rejected", !bad.valid);

  base::Vector<u8> truncated(blob.begin(), blob.begin() + 40);
  rx::bethesda::ShaderPackage cut =
      rx::bethesda::ParseShaderPackage(rx::ByteSpan(truncated.data(), truncated.size()));
  Check("truncated blob rejected", !cut.valid);
}

// Fallout 4 kept the record shape but grew the group header to five slots
// (vertex, hull, domain, pixel, compute) and re-sized the descriptors.
void TestFallout4Fxp() {
  std::puts("fxp (Fallout 4 five-slot groups)");

  auto descriptor = [](size_t size, u8 marker) {
    base::Vector<u8> d;
    for (size_t i = 0; i < size; ++i)
      d.push_back(0xff);  // the shipped tables pad unused entries with 0xff
    d[0] = marker;
    return d;
  };

  base::Vector<u8> blob;
  // Group 0: one vertex, one pixel.
  PutU32(blob, 1);
  PutU32(blob, 0);
  PutU32(blob, 0);
  PutU32(blob, 1);
  PutU32(blob, 0);
  PutFxpRecord(blob, 0x00000000, descriptor(44, 0x11), MakeDxbc(1, 4));
  PutFxpRecord(blob, 0x00000002, descriptor(36, 0x22), MakeDxbc(0, 5));
  // Group 1: the tessellation and compute slots, which Skyrim SE had no room
  // for in its header.
  PutU32(blob, 0);
  PutU32(blob, 1);
  PutU32(blob, 1);
  PutU32(blob, 0);
  PutU32(blob, 1);
  PutFxpRecord(blob, 0x00000003, descriptor(44, 0x33), MakeDxbc(3, 3));
  PutFxpRecord(blob, 0x00000004, descriptor(44, 0x44), MakeDxbc(4, 3));
  PutFxpRecord(blob, 0x00000005, descriptor(44, 0x55), MakeDxbc(5, 3));

  rx::bethesda::ShaderPackage p =
      rx::bethesda::ParseShaderPackage(rx::ByteSpan(blob.data(), blob.size()));

  Check("parses valid", p.valid);
  Check("five shaders", p.shaders.size() == 5);
  Check("two groups", p.groups.size() == 2);
  if (p.shaders.size() != 5 || p.groups.size() != 2)
    return;
  Check("vertex descriptor is 44 bytes", p.shaders[0].descriptor.size() == 44);
  Check("pixel descriptor is 36 bytes", p.shaders[1].descriptor.size() == 36);
  Check("hull stage", p.shaders[2].stage == ShaderStage::kHull);
  Check("domain stage", p.shaders[3].stage == ShaderStage::kDomain);
  Check("compute stage", p.shaders[4].stage == ShaderStage::kCompute);
  Check("hull count recorded", p.groups[1].hull_count == 1 && p.groups[1].domain_count == 1 &&
                                   p.groups[1].compute_count == 1);
  Check("slot order preserved", p.shaders[2].descriptor.data()[0] == 0x33 &&
                                    p.shaders[4].descriptor.data()[0] == 0x55);
}

void TestSdp() {
  std::puts("sdp (Oblivion / Fallout 3 / New Vegas shaderpackage)");

  base::Vector<u8> code_vs = MakeD3d9(0xfffe0300, 12);
  base::Vector<u8> code_ps = MakeD3d9(0xffff0200, 20);

  base::Vector<u8> records;
  PutSdpRecord(records, "SLS1000.vso", code_vs);
  PutSdpRecord(records, "WATER.pso", code_ps);

  base::Vector<u8> blob;
  PutU32(blob, 100);  // version
  PutU32(blob, 2);    // shader count
  PutU32(blob, static_cast<u32>(records.size()));
  PutBytes(blob, records.data(), records.size());

  rx::bethesda::ShaderPackage p =
      rx::bethesda::ParseShaderPackage(rx::ByteSpan(blob.data(), blob.size()));

  Check("parses valid", p.valid);
  Check("two shaders", p.shaders.size() == 2);
  if (p.shaders.size() != 2)
    return;
  Check("name kept", p.shaders[0].name == "SLS1000.vso");
  Check("second name kept", p.shaders[1].name == "WATER.pso");
  Check("vertex stage from d3d9 token", p.shaders[0].stage == ShaderStage::kVertex);
  Check("pixel stage from d3d9 token", p.shaders[1].stage == ShaderStage::kPixel);
  Check("bytecode size", p.shaders[1].bytecode.size() == code_ps.size());
  Check("no fxp groups", p.groups.empty());

  base::Vector<u8> truncated(blob.begin(), blob.begin() + 300);
  rx::bethesda::ShaderPackage cut =
      rx::bethesda::ParseShaderPackage(rx::ByteSpan(truncated.data(), truncated.size()));
  Check("truncated blob rejected", !cut.valid);
}

// A DXBC container whose token stream carries real declarations, so the
// reflection has something to count.
base::Vector<u8> MakeDxbcWithDecls(u32 program_type,
                                   u32 textures,
                                   u32 samplers,
                                   u32 cb_slot_a,
                                   u32 cb_vectors_a,
                                   u32 cb_slot_b,
                                   u32 cb_vectors_b,
                                   u32 instructions) {
  base::Vector<u8> tokens;
  auto decl_cb = [&](u32 slot, u32 vectors) {
    PutU32(tokens, (4u << 24) | 89);  // dcl_constantbuffer, 4 dwords
    PutU32(tokens, 0x00208006);       // operand: cb file, two indices
    PutU32(tokens, slot);
    PutU32(tokens, vectors);
  };
  decl_cb(cb_slot_a, cb_vectors_a);
  decl_cb(cb_slot_b, cb_vectors_b);
  for (u32 i = 0; i < samplers; ++i) {
    PutU32(tokens, (3u << 24) | 90);  // dcl_sampler, 3 dwords
    PutU32(tokens, 0x00006000);
    PutU32(tokens, i);
  }
  for (u32 i = 0; i < textures; ++i) {
    PutU32(tokens, (4u << 24) | 88);  // dcl_resource, 4 dwords
    PutU32(tokens, 0x00107000);
    PutU32(tokens, i);
    PutU32(tokens, 0x5555);  // return type
  }
  for (u32 i = 0; i < instructions; ++i) {
    PutU32(tokens, (2u << 24) | 1);  // a two-dword core instruction
    PutU32(tokens, 0);
  }

  base::Vector<u8> b;
  PutBytes(b, "DXBC", 4);
  for (int i = 0; i < 16; ++i)
    b.push_back(u8(0xb0 + i));
  PutU32(b, 1);
  PutU32(b, 0);  // total size, patched below
  PutU32(b, 1);
  PutU32(b, 0x24);
  PutBytes(b, "SHEX", 4);
  PutU32(b, 8 + static_cast<u32>(tokens.size()));
  PutU32(b, (program_type << 16) | 0x50);
  PutU32(b, 2 + static_cast<u32>(tokens.size()) / 4);
  PutBytes(b, tokens.data(), tokens.size());

  const u32 total = static_cast<u32>(b.size());
  std::memcpy(b.data() + 0x18, &total, 4);
  return b;
}

void TestReflection() {
  std::puts("reflection");

  base::Vector<u8> dxbc = MakeDxbcWithDecls(/*program_type=*/0, /*textures=*/3, /*samplers=*/2,
                                            /*cb_slot_a=*/1, /*cb_vectors_a=*/3,
                                            /*cb_slot_b=*/12, /*cb_vectors_b=*/20,
                                            /*instructions=*/7);
  rx::bethesda::ShaderReflection r =
      rx::bethesda::ReflectShader(rx::ByteSpan(dxbc.data(), dxbc.size()));

  Check("reflects", r.valid);
  Check("textures counted", r.textures == 3);
  Check("samplers counted", r.samplers == 2);
  Check("constant buffers counted", r.constant_buffers == 2);
  Check("slots as a mask", r.constant_buffer_slots == ((1u << 1) | (1u << 12)));
  Check("float4s summed", r.constant_buffer_vectors == 23);
  Check("instructions counted", r.instructions == 7);

  // D3D9 bytecode carries none of this.
  base::Vector<u8> d3d9 = MakeD3d9(0xffff0200, 8);
  rx::bethesda::ShaderReflection none =
      rx::bethesda::ReflectShader(rx::ByteSpan(d3d9.data(), d3d9.size()));
  Check("d3d9 bytecode not reflected", !none.valid);
}

void TestRejects() {
  std::puts("rejects");
  base::Vector<u8> junk;
  for (int i = 0; i < 64; ++i)
    junk.push_back(u8(i));
  rx::bethesda::ShaderPackage p =
      rx::bethesda::ParseShaderPackage(rx::ByteSpan(junk.data(), junk.size()));
  Check("unknown container rejected", !p.valid && p.shaders.empty());
  rx::bethesda::ShaderPackage empty = rx::bethesda::ParseShaderPackage(rx::ByteSpan());
  Check("empty blob rejected", !empty.valid);
}

}  // namespace

int main() {
  std::puts("shader_packagetest");
  TestFxp();
  TestFallout4Fxp();
  TestSdp();
  TestReflection();
  TestRejects();

  if (g_failures == 0) {
    std::puts("shader_package: all checks passed");
    return 0;
  }
  std::printf("shader_package: %d checks FAILED\n", g_failures);
  return 1;
}
