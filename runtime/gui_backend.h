#ifndef RECREATION_RUNTIME_GUI_BACKEND_H_
#define RECREATION_RUNTIME_GUI_BACKEND_H_

#include <volk.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <ugui/render/draw_data.h>
#include <ugui/render/texture_backend.h>
#include <ugui/rhi/rhi_types.h>

namespace rx::ui {

// Vulkan renderer backend for ultragui draw data. Adapted from the bundled
// ugui_impl_vulkan to the recreation engine's conventions: volk entry points,
// dynamic rendering (no VkRenderPass) and shaders embedded as SPIR-V at build
// time. It records into a command buffer the engine already opened with
// vkCmdBeginRendering on the backbuffer, exactly where the debug ImGui overlay
// records. Also serves as ultragui's TextureBackend so Image/SVG textures work
// in draw-data mode.
class GuiRenderBackend final : public ugui::TextureBackend {
 public:
  struct InitInfo {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkFormat color_format = VK_FORMAT_UNDEFINED;  // backbuffer / swapchain format
    uint32_t frames_in_flight = 2;
  };

  GuiRenderBackend() = default;
  ~GuiRenderBackend() override = default;
  GuiRenderBackend(const GuiRenderBackend&) = delete;
  GuiRenderBackend& operator=(const GuiRenderBackend&) = delete;

  bool Init(const InitInfo& info);
  void Shutdown();

  // Advance the per-frame vertex/index ring. Call once per frame before Render.
  void NewFrame();

  // (Re)upload the glyph atlas (R8 alpha). Call when TextEngine::atlas_revision
  // changes. Stalls the device, so call outside a render pass.
  bool UpdateFontAtlas(const uint8_t* pixels, uint32_t width, uint32_t height);

  // Record the draw list into `cmd`, inside a render pass the host began with a
  // color attachment of InitInfo::color_format.
  void Render(const ugui::DrawData& draw_data, VkCommandBuffer cmd);

  // Provide the pre-blurred backdrop (a Gaussian-blurred copy of what is behind
  // the UI) that frosted-glass quads (DrawCmd::blur > 0) sample. Pass a null
  // view to disable; then blur commands fall back to the plain quad pipeline.
  // Set once per frame before Render, with a view valid for that submission.
  void SetBackdrop(VkImageView view, VkSampler sampler);

  // ugui::TextureBackend.
  ugui::TextureId CreateTexture(uint32_t width, uint32_t height, ugui::RHIFormat format,
                                const void* pixels, ugui::RHIFilter filter) override;
  void UpdateTexture(ugui::TextureId id, const void* pixels) override;
  void DestroyTexture(ugui::TextureId id) override;

 private:
  struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize capacity = 0;
  };
  struct FrameBuffers {
    GpuBuffer quad_vtx, quad_idx, text_vtx, text_idx;
  };
  struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };
  struct UserTexture {
    Texture tex;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t pixel_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VkSampler sampler = VK_NULL_HANDLE;
  };

  uint32_t FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags props) const;
  void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                    GpuBuffer& out);
  void DestroyBuffer(GpuBuffer& b);
  void UploadBuffer(GpuBuffer& b, VkBufferUsageFlags usage, const void* src, VkDeviceSize bytes);
  VkPipeline CreatePipeline(const unsigned char* vs, size_t vs_size, const unsigned char* fs,
                            size_t fs_size, uint32_t attr_count);
  Texture MakeTexture(uint32_t w, uint32_t h, VkFormat fmt, uint32_t pixel_size,
                      const void* pixels, VkSampler sampler);
  void FreeTexture(Texture& t);
  VkSampler MakeSampler(VkFilter filter);

  InitInfo info_{};
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline quad_pipeline_ = VK_NULL_HANDLE;
  VkPipeline text_pipeline_ = VK_NULL_HANDLE;
  VkPipeline frost_pipeline_ = VK_NULL_HANDLE;  // backdrop-blur (frosted glass)
  VkSampler linear_sampler_ = VK_NULL_HANDLE;
  VkSampler nearest_sampler_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  Texture white_{};
  Texture font_{};
  std::unordered_map<ugui::TextureId, UserTexture> user_textures_;
  ugui::TextureId next_user_id_ = 1;
  std::vector<FrameBuffers> frames_;
  uint32_t frame_index_ = 0;

  // Backdrop-blur (frosted glass): the renderer hands a blurred copy of what is
  // behind the UI each frame; frost commands sample it via a per-frame set.
  VkImageView backdrop_view_ = VK_NULL_HANDLE;
  VkSampler backdrop_sampler_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> frost_sets_;
};

}  // namespace rx::ui

#endif  // RECREATION_RUNTIME_GUI_BACKEND_H_
