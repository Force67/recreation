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
  static std::unique_ptr<PostPass> Create(Device& device, Format output_format);
  ~PostPass();

  PostPass(const PostPass&) = delete;
  PostPass& operator=(const PostPass&) = delete;

  struct Params {
    u32 tonemap = 0;  // TonemapOperator
    f32 bloom_intensity = 0.04f;
    u32 bloom_enabled = 0;
    u32 lut_enabled = 0;  // sample the grading lut (0 = neutral, skip)
    // Pushed verbatim to tonemap.ps: output encode (0 srgb, 1 hdr10 pq,
    // 2 scrgb) + the nit level tonemapped white maps to in the HDR modes.
    u32 output_transfer = 0;
    f32 paper_white = 200.0f;
    f32 pad0 = 0;
    f32 pad1 = 0;
  };

  // Rebakes the grading strip LUT when the grade changes (no-op otherwise).
  void SetGrade(ColorGrade grade);

  // Loads a Resolve/Adobe .cube 3D lut from disk, resampled into the strip lut.
  // Leaves the grade marked kCustom so SetGrade does not bake over it. Returns
  // false (and keeps the current lut) on a parse or read error.
  bool LoadCubeLut(const std::string& path);

  // bloom may be the input view when bloom is off (still bound, not read).
  void Record(PassContext& ctx, TextureView input, TextureView bloom, const GpuBuffer& exposure,
              u64 exposure_size, TextureView output, Extent2D output_extent,
              const Params& params);

 private:
  explicit PostPass(Device& device) : device_(device) {}

  // A 32^3 strip lut: blue slices laid out horizontally, 1024x32 rgba8.
  static constexpr u32 kLutSize = 32;
  bool CreateLut();
  void UploadLut(ColorGrade grade);
  void UploadLutPixels(base::Vector<u8>& pixels);  // staging + copy of a 32^3 strip

  Device& device_;
  SamplerHandle sampler_;
  PipelineHandle pipeline_;
  GpuImage lut_;
  ColorGrade lut_grade_ = ColorGrade::kNeutral;
  bool lut_ready_ = false;  // false until the first upload transitions it
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_POST_H_
