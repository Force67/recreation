#ifndef RECREATION_RENDER_RHI_RESOURCES_H_
#define RECREATION_RENDER_RHI_RESOURCES_H_

#include <volk.h>

#include <vk_mem_alloc.h>

#include <base/containers/vector.h>

#include "core/types.h"

namespace rec::render {

struct GpuBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  u64 size = 0;
  void* mapped = nullptr;  // set for host visible buffers
};

struct GpuImage {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkExtent2D extent{};
};

// Index range drawn with one material. Materials resolve at upload time so
// the draw loop is a plain array walk.
struct GpuSubmesh {
  u32 index_offset = 0;
  u32 index_count = 0;
  u64 material = 0;  // material asset hash, 0 = default material
  bool blend = false;  // alpha blended: drawn sorted after opaque
  bool water = false;  // routed to the water pipeline
};

struct GpuMesh {
  GpuBuffer vertices;
  GpuBuffer indices;
  u32 index_count = 0;
  u32 vertex_count = 0;
  bool all_blend = false;  // pure transparency (water): stays out of the tlas
  bool no_rt = false;      // grass-like fill geometry, excluded from the tlas
  u32 bindless_index = 0;  // mesh record in the bindless registry
  base::Vector<GpuSubmesh> submeshes;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_RESOURCES_H_
