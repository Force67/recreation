#ifndef RECREATION_RENDER_POST_H_
#define RECREATION_RENDER_POST_H_

#include <memory>
#include <string>

#include <base/containers/vector.h>

#include "render/core/render_graph.h"
#include "render/rhi/device.h"
#include "render/core/settings.h"

namespace rec::render {

// Final pass of the frame: samples the resolved hdr image, tonemaps,
// applies the srgb transfer function and writes the backbuffer. Any gap
// between render and output resolution is absorbed here by the linear
// sampler until a real upscaler owns it.
class PostPass {
 public:
  static std::unique_ptr<PostPass> Create(Device& device, VkFormat output_format);
  ~PostPass();

  PostPass(const PostPass&) = delete;
  PostPass& operator=(const PostPass&) = delete;

  struct Params {
    u32 tonemap = 0;  // TonemapOperator
    f32 bloom_intensity = 0.04f;
    u32 bloom_enabled = 0;
    u32 lut_enabled = 0;  // sample the grading lut (0 = neutral, skip)
  };

  // Rebakes the grading strip LUT when the grade changes (no-op otherwise).
  void SetGrade(ColorGrade grade);

  // Loads a Resolve/Adobe .cube 3D lut from disk, resampled into the strip lut.
  // Leaves the grade marked kCustom so SetGrade does not bake over it. Returns
  // false (and keeps the current lut) on a parse or read error.
  bool LoadCubeLut(const std::string& path);

  // bloom may be the input view when bloom is off (still bound, not read).
  void Record(PassContext& ctx, VkImageView input, VkImageView bloom, VkBuffer exposure,
              u64 exposure_size, VkImageView output, VkExtent2D output_extent,
              const Params& params);

 private:
  explicit PostPass(Device& device) : device_(device) {}

  // A 32^3 strip lut: blue slices laid out horizontally, 1024x32 rgba8.
  static constexpr u32 kLutSize = 32;
  bool CreateLut();
  void UploadLut(ColorGrade grade);
  void UploadLutPixels(base::Vector<u8>& pixels);  // staging + copy of a 32^3 strip

  Device& device_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  GpuImage lut_;
  ColorGrade lut_grade_ = ColorGrade::kNeutral;
  bool lut_ready_ = false;  // false until the first upload transitions it
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_POST_H_
