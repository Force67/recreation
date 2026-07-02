#ifndef RECREATION_RENDER_RHI_BINDINGS_H_
#define RECREATION_RENDER_RHI_BINDINGS_H_

#include <base/containers/vector.h>

#include "core/types.h"
#include "render/rhi/resources.h"
#include "render/rhi/types.h"

namespace rec::render {

// Resource classes a shader can bind. Maps to Vulkan descriptor types and
// D3D12 UAV/SRV/CBV/sampler ranges. kCombinedTextureSampler exists because the
// HLSL uses [[vk::combinedImageSampler]]; on non-Vulkan backends it lowers to
// a texture + sampler pair at the same slot.
enum class BindingType : u8 {
  kStorageImage,
  kSampledImage,
  kCombinedTextureSampler,
  kSampler,
  kUniformBuffer,
  kStorageBuffer,
  kAccelStruct,
};

// One shader-visible slot in a set. `count` > 1 declares an array;
// `variable_count` marks the trailing bindless array (partially bound,
// update-after-bind).
struct BindingSlot {
  u32 binding = 0;
  BindingType type = BindingType::kSampledImage;
  u32 count = 1;
  bool variable_count = false;
};

struct BindingLayoutDesc {
  ShaderStageFlags stages = kShaderStageCompute;
  base::Vector<BindingSlot> slots;
  bool update_after_bind = false;  // bindless registries; costs a heavier pool
};

// One resource bound to a slot, for transient (per-record) binding or
// persistent BindingSet updates. Use the factory helpers; the raw fields are
// backend plumbing.
struct BindingItem {
  u32 slot = 0;
  BindingType type = BindingType::kSampledImage;
  u32 array_index = 0;
  TextureView view;
  SamplerHandle sampler;
  BufferHandle buffer;
  u64 buffer_offset = 0;
  u64 buffer_range = 0;  // 0 = whole buffer
  AccelStructHandle accel;
  // Sampled/combined bindings normally expect the image in the shader-read
  // state; storage-persistent images (probe atlases living in kGeneral) set
  // this so the descriptor matches their actual state.
  bool general_layout = false;
};

// Marks a sampled/combined item as reading an image that lives in kGeneral.
inline BindingItem InGeneral(BindingItem item) {
  item.general_layout = true;
  return item;
}

// Factories keep call sites one-liners:
//   cmd->BindTransient(0, {Bind::Storage(0, img), Bind::Sampled(1, depth)});
namespace Bind {

inline BindingItem Storage(u32 slot, const GpuImage& image) {
  return {.slot = slot, .type = BindingType::kStorageImage, .view = image.view};
}
inline BindingItem StorageView(u32 slot, TextureView view) {
  return {.slot = slot, .type = BindingType::kStorageImage, .view = view};
}
inline BindingItem Sampled(u32 slot, const GpuImage& image) {
  return {.slot = slot, .type = BindingType::kSampledImage, .view = image.view};
}
inline BindingItem SampledView(u32 slot, TextureView view) {
  return {.slot = slot, .type = BindingType::kSampledImage, .view = view};
}
inline BindingItem Combined(u32 slot, TextureView view, SamplerHandle sampler) {
  return {.slot = slot, .type = BindingType::kCombinedTextureSampler, .view = view,
          .sampler = sampler};
}
inline BindingItem Sampler(u32 slot, SamplerHandle sampler) {
  return {.slot = slot, .type = BindingType::kSampler, .sampler = sampler};
}
inline BindingItem Uniform(u32 slot, const GpuBuffer& buffer, u64 offset = 0, u64 range = 0) {
  return {.slot = slot, .type = BindingType::kUniformBuffer, .buffer = buffer.handle,
          .buffer_offset = offset, .buffer_range = range};
}
inline BindingItem StorageBuffer(u32 slot, const GpuBuffer& buffer, u64 offset = 0,
                                 u64 range = 0) {
  return {.slot = slot, .type = BindingType::kStorageBuffer, .buffer = buffer.handle,
          .buffer_offset = offset, .buffer_range = range};
}
inline BindingItem Accel(u32 slot, AccelStructHandle accel) {
  return {.slot = slot, .type = BindingType::kAccelStruct, .accel = accel};
}

}  // namespace Bind

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_BINDINGS_H_
