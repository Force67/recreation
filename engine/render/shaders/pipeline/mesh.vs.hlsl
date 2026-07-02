#include "rhi_bindings.hlsli"
struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  column_major float4x4 inv_view_proj;
  float2 jitter;  // ndc units, applied on top of the unjittered clip pos
  float2 prev_jitter;
  float4 sun_direction;  // xyz travel direction of the light, w intensity
  float4 sun_color;      // rgb color, w flat ambient when ibl is off
  float4 camera_position;  // xyz eye, w ibl intensity
  float4 misc;             // x,y render size, z sun angular radius, w frame index
  uint flags;
  float3 pad;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame : register(b0, space0);

struct PushData {
  column_major float4x4 model;
  column_major float4x4 prev_model;
#ifdef REC_SKINNED
  uint64_t bone_address;  // device address of the frame bone palette
  uint skin_offset;       // first bone of this mesh in the palette
  uint tint_packed;       // rgb8 (0xRRGGBB) albedo tint, 0 = none (team colour)
#endif
};
PUSH_CONSTANTS(PushData, push);

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(1)]] float3 normal : NORMAL;
  [[vk::location(2)]] float4 tangent : TANGENT;
  [[vk::location(3)]] float2 uv : TEXCOORD0;
  [[vk::location(4)]] float4 color : COLOR0;
#ifdef REC_SKINNED
  [[vk::location(5)]] uint4 bone_indices : BLENDINDICES0;
  [[vk::location(6)]] float4 bone_weights : BLENDWEIGHT0;  // unorm 0..1
#endif
};

struct VsOut {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float4 curr_clip : TEXCOORD1;
  [[vk::location(2)]] float4 prev_clip : TEXCOORD2;
  [[vk::location(3)]] float3 world_pos : TEXCOORD3;
  [[vk::location(4)]] float4 tangent : TANGENT;
  [[vk::location(5)]] float2 uv : TEXCOORD0;
  [[vk::location(6)]] float4 color : COLOR0;
};

#ifdef REC_SKINNED
#ifndef __spirv__
// DXIL has no buffer-device-address loads: the d3d12 backend binds the bone
// palette as a root SRV at (t998, space0) using the u64 address it finds at
// byte 128 of the push block (the REC_BDA convention, see RHI.md). SPIR-V
// keeps reading through the raw address.
ByteAddressBuffer rec_bone_palette : register(t998, space0);
#endif

// Each bone is a column-major 4x4 (64 bytes) in the palette, so M*v is the
// weighted sum of columns. Normals/tangents use the upper 3x3.
void SkinVertex(VsIn input, out float3 position, out float3 normal, out float3 tangent) {
  position = float3(0, 0, 0);
  normal = float3(0, 0, 0);
  tangent = float3(0, 0, 0);
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
    normal += w * (c0.xyz * input.normal.x + c1.xyz * input.normal.y + c2.xyz * input.normal.z);
    tangent +=
        w * (c0.xyz * input.tangent.x + c1.xyz * input.tangent.y + c2.xyz * input.tangent.z);
  }
}
#endif

VsOut main(VsIn input) {
  VsOut output;
  float3 local_pos = input.position;
  float3 local_normal = input.normal;
  float3 local_tangent = input.tangent.xyz;
#ifdef REC_SKINNED
  SkinVertex(input, local_pos, local_normal, local_tangent);
#endif
  float4 world = mul(push.model, float4(local_pos, 1.0));
  float4 clip = mul(frame.view_proj, world);
  output.world_pos = world.xyz;
  // Motion vectors compare unjittered positions, so jitter only moves the
  // rasterized sample, never the reprojection. Skinned deformation reuses the
  // current pose for prev (rigid motion only).
  output.curr_clip = clip;
  output.prev_clip = mul(frame.prev_view_proj, mul(push.prev_model, float4(local_pos, 1.0)));
  output.sv_position = clip + float4(frame.jitter * clip.w, 0.0, 0.0);
  output.normal = mul((float3x3)push.model, local_normal);
  output.tangent = float4(mul((float3x3)push.model, local_tangent), input.tangent.w);
  output.uv = input.uv;
  output.color = input.color;
#ifdef REC_SKINNED
  // Modulate the actor's albedo by its packed team/faction tint so the two
  // armies read apart (and the lower-than-1 factors also tame the snow blowout).
  if (push.tint_packed != 0u) {
    float3 t = float3(float((push.tint_packed >> 16) & 0xffu),
                      float((push.tint_packed >> 8) & 0xffu),
                      float(push.tint_packed & 0xffu)) /
               255.0;
    output.color.rgb *= t;
  }
#endif
  return output;
}
