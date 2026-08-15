#ifndef RECREATION_BETHESDA_SHADER_PACKAGE_H_
#define RECREATION_BETHESDA_SHADER_PACKAGE_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace rx::bethesda {

// The shipped shader collections. Every Bethesda game since Oblivion keeps its
// whole compiled shader library in one blob:
//
//   .sdp  Oblivion/FO3/FNV  Data/Shaders/shaderpackage###.sdp, D3D9 bytecode,
//         and the only format that keeps NAMES ("SLS1000.vso", "WATER.pso").
//   .fxp  Skyrim SE/FO4     shadersfx/shaders011.fxp inside the Shaders
//         archive, DXBC (SM5) with the reflection chunk stripped.
//
// The .fxp reflection strip is why the per-shader descriptor below matters: the
// blobs carry no cbuffer or texture names, so the runtime binds constants
// through these tables instead. Everything here is what the bytes say; the
// meaning of individual technique bits is the caller's problem.
//
// Layouts, each verified by walking a shipped package end to end with no bytes
// left over (Skyrim SE 16034 shaders / 135 groups, Fallout 4 3939 / 186,
// Fallout 3 and Oblivion 500-1000 per package):
//
//   .fxp  a flat run of groups, one per shader class, no names or count up
//         front. A group header is a list of per-slot counts, followed by that
//         many records per slot in order:
//           Skyrim SE   u32 vertexCount, u32 pixelCount
//                       except compute groups, which carry a single count and
//                       are told apart because the record magic sits where the
//                       second count would be
//           Fallout 4   u32 vertex, hull, domain, pixel, compute
//         and a record is
//           u32 magic (0x11223344), u32 bytecodeSize, u32 techniqueId,
//           u8 descriptor[fixed per slot], u8 dxbc[bytecodeSize]
//         Descriptor sizes differ per game: Skyrim SE 32 vertex / 68 pixel /
//         36 compute, Fallout 4 44 for every stage but pixel, which is 36.
//         Unused table entries read 0x00 on Skyrim SE and 0xff on Fallout 4.
//
//   .sdp  u32 version (100), u32 shaderCount, u32 recordBytes, then records of
//         char name[256] (NUL-terminated, padded with 0xfd), u32 size,
//         u8 bytecode[size].
//
// Starfield ships a third container (shaderspc_dx12_105.fxp) that opens with a
// build hash and names its shader classes; it is not decoded yet.
enum class ShaderStage : u8 {
  kVertex,
  kPixel,
  kGeometry,
  kHull,
  kDomain,
  kCompute,
  kUnknown,
};

const char* ShaderStageName(ShaderStage stage);

struct PackagedShader {
  ShaderStage stage = ShaderStage::kUnknown;
  // Index of the containing group (.fxp) — one group per shader class, in the
  // order the engine registers them. Always 0 for .sdp.
  u32 group = 0;
  // Permutation key within the group. The engine picks a shader by matching
  // this against the flags a material asks for.
  u32 technique_id = 0;
  // .sdp only; .fxp shipped no names.
  base::String name;
  // .fxp vertex shaders: the packed vertex-layout description. Its low 12 bits
  // track which attributes the shader consumes.
  u64 vertex_desc = 0;
  // Raw per-shader constant table, fixed at 32B for a vertex shader, 68B for a
  // pixel shader and 36B for a compute shader. The entries are float offsets
  // into the constant buffers the shader declares, grouped per buffer.
  ByteSpan descriptor;
  // Compiled bytecode: a DXBC container (.fxp) or D3D9 bytecode (.sdp). Aliases
  // the blob passed to the parser.
  ByteSpan bytecode;
};

// One shader class worth of permutations (.fxp only). Skyrim SE groups pair a
// vertex and a pixel count, except compute groups which carry a single count;
// Fallout 4 groups always carry five, having gained the tessellation stages.
// The records follow in slot order.
struct ShaderGroup {
  u32 vertex_count = 0;
  u32 hull_count = 0;
  u32 domain_count = 0;
  u32 pixel_count = 0;
  u32 compute_count = 0;
  u32 first_shader = 0;  // index into ShaderPackage::shaders
  u32 shader_count = 0;
};

struct ShaderPackage {
  base::Vector<PackagedShader> shaders;
  base::Vector<ShaderGroup> groups;
  bool valid = false;
};

// Parses either container, dispatching on the header. Returns valid=false on
// any structural error; shaders decoded before the error are kept so a
// truncated or partly-understood package still reports what it had.
ShaderPackage ParseShaderPackage(ByteSpan data);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_SHADER_PACKAGE_H_
