#ifndef RECREATION_RENDER_SHADER_UTIL_H_
#define RECREATION_RENDER_SHADER_UTIL_H_

// Vulkan-backend-only utility (compiled under RECREATION_RHI_VULKAN). Pass
// code never needs this; it exists for interop modules that build their own
// Vulkan pipelines (NRD, the runtime gui backend, the thumbnailer).

#include <volk.h>

#include <cstddef>

namespace rec::render {

// Wraps an embedded spirv blob. The embedded arrays are byte aligned,
// spirv wants words, so this copies. Returns VK_NULL_HANDLE on failure.
VkShaderModule CreateShaderModule(VkDevice device, const unsigned char* code, size_t size);

}  // namespace rec::render

#endif  // RECREATION_RENDER_SHADER_UTIL_H_
