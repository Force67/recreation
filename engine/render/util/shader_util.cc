#include "render/util/shader_util.h"

#include <cstring>

#include <base/containers/vector.h>

#include "core/types.h"

namespace rec::render {

VkShaderModule CreateShaderModule(VkDevice device, const unsigned char* code, size_t size) {
  base::Vector<u32> words((size + 3) / 4);
  std::memcpy(words.data(), code, size);

  VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = size;
  info.pCode = words.data();
  VkShaderModule module = VK_NULL_HANDLE;
  vkCreateShaderModule(device, &info, nullptr, &module);
  return module;
}

}  // namespace rec::render
