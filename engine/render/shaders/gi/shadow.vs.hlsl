#include "rhi_bindings.hlsli"
// Depth-only cascade shadow caster. Renders opaque geometry from the sun into
// one sub-rect of the cascade atlas. The uv is passed through so the fragment
// stage can alpha-test masked materials (perforated foliage shadows); fully
// opaque casters never sample it. One draw per cascade, the light matrix
// arrives per draw. REC_SKINNED adds the bone weight stream so animated meshes
// cast a pose-matched shadow instead of their bind pose.
struct PushData {
  column_major float4x4 light_view_proj;
  column_major float4x4 model;
#ifdef REC_SKINNED
  uint64_t bone_address;  // device address of the frame bone palette
  uint skin_offset;       // first bone of this mesh in the palette
  uint pad;
#endif
};
PUSH_CONSTANTS(PushData, push);

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(3)]] float2 uv : TEXCOORD0;
#ifdef REC_SKINNED
  [[vk::location(5)]] uint4 bone_indices : BLENDINDICES0;
  [[vk::location(6)]] float4 bone_weights : BLENDWEIGHT0;  // unorm 0..1
#endif
};

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

#ifdef REC_SKINNED
#ifndef __spirv__
// DXIL bone palette access: root SRV bound by the d3d12 backend from the
// address at push-block byte 128 (REC_BDA convention, see mesh.vs / RHI.md).
ByteAddressBuffer rec_bone_palette : register(t998, space0);
#endif

// Same bone palette layout as mesh_skin.vs: each bone a column-major 4x4.
float3 SkinPosition(VsIn input) {
  float3 position = float3(0, 0, 0);
  float4 p = float4(input.position, 1.0);
  [unroll]
  for (uint i = 0; i < 4; ++i) {
    float w = input.bone_weights[i];
    if (w <= 0.0) continue;
    uint bone_byte_offset = (push.skin_offset + input.bone_indices[i]) * 64;
#ifdef __spirv__
    uint64_t addr = push.bone_address + (uint64_t)bone_byte_offset;
    float4 c0 = vk::RawBufferLoad<float4>(addr + 0);
    float4 c1 = vk::RawBufferLoad<float4>(addr + 16);
    float4 c2 = vk::RawBufferLoad<float4>(addr + 32);
    float4 c3 = vk::RawBufferLoad<float4>(addr + 48);
#else
    float4 c0 = asfloat(rec_bone_palette.Load4(bone_byte_offset + 0));
    float4 c1 = asfloat(rec_bone_palette.Load4(bone_byte_offset + 16));
    float4 c2 = asfloat(rec_bone_palette.Load4(bone_byte_offset + 32));
    float4 c3 = asfloat(rec_bone_palette.Load4(bone_byte_offset + 48));
#endif
    position += w * (c0 * p.x + c1 * p.y + c2 * p.z + c3 * p.w).xyz;
  }
  return position;
}
#endif

VsOut main(VsIn input) {
  float3 local_pos = input.position;
#ifdef REC_SKINNED
  local_pos = SkinPosition(input);
#endif
  float4 world = mul(push.model, float4(local_pos, 1.0));
  VsOut o;
  o.pos = mul(push.light_view_proj, world);
  o.uv = input.uv;
  return o;
}
