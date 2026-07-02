#include "render/post/post.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include <base/containers/vector.h>

#include "core/log.h"
#include "shaders/fullscreen_vs_hlsl.h"
#include "shaders/tonemap_ps_hlsl.h"

namespace rec::render {

std::unique_ptr<PostPass> PostPass::Create(Device& device, Format output_format) {
  auto pass = std::unique_ptr<PostPass>(new PostPass(device));

  pass->sampler_ = device.GetSampler({.address_u = AddressMode::kClampToEdge,
                                      .address_v = AddressMode::kClampToEdge,
                                      .address_w = AddressMode::kClampToEdge});

  pass->pipeline_ = device.CreateGraphicsPipeline({
      .vertex = REC_SHADER(k_fullscreen_vs_hlsl),
      .fragment = REC_SHADER(k_tonemap_ps_hlsl),
      .raster = {.cull = CullMode::kNone},
      .color_formats = {output_format},
      .blend = {BlendMode::kOpaque},
      .sets = {{.slots = {{0, BindingType::kCombinedTextureSampler},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kCombinedTextureSampler}},  // grading strip lut
                .stages = kShaderStageFragment}},
      .push_constant_size = sizeof(Params),
      .debug_name = "post_tonemap",
  });
  if (!pass->pipeline_) {
    REC_ERROR("post pipeline creation failed");
    return nullptr;
  }
  if (!pass->CreateLut()) return nullptr;
  return pass;
}

namespace {

f32 Clamp01(f32 x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Applies a built-in grade to a neutral display-space color (tonemapped,
// pre-srgb). kNeutral is identity. Authored to read clearly: warm/cool shift
// white balance, cinematic does a teal-shadow / orange-highlight split with a
// touch of contrast.
void ApplyGrade(ColorGrade grade, f32 in[3], f32 out[3]) {
  f32 r = in[0], g = in[1], b = in[2];
  switch (grade) {
    case ColorGrade::kNeutral:
      break;
    case ColorGrade::kWarm:
      r *= 1.10f;
      g *= 1.02f;
      b *= 0.88f;
      break;
    case ColorGrade::kCool:
      r *= 0.88f;
      g *= 1.0f;
      b *= 1.12f;
      break;
    case ColorGrade::kCinematic: {
      f32 luma = 0.299f * r + 0.587f * g + 0.114f * b;
      // Teal in shadows, orange in highlights, lerped by luma.
      r += (1.0f - luma) * -0.04f + luma * 0.10f;
      g += (1.0f - luma) * 0.03f + luma * 0.04f;
      b += (1.0f - luma) * 0.10f + luma * -0.07f;
      r = (r - 0.5f) * 1.12f + 0.5f;  // gentle contrast
      g = (g - 0.5f) * 1.12f + 0.5f;
      b = (b - 0.5f) * 1.12f + 0.5f;
      break;
    }
    case ColorGrade::kCustom:  // filled from a loaded .cube, not procedural
      break;
  }
  out[0] = Clamp01(r);
  out[1] = Clamp01(g);
  out[2] = Clamp01(b);
}

// A parsed Resolve/Adobe .cube 3D lut: size^3 rgb triples, red varying fastest.
struct CubeLut {
  u32 size = 0;
  base::Vector<f32> rgb;  // size^3 * 3
};

// Parses LUT_3D_SIZE + the triple list. Ignores comments, TITLE and DOMAIN_*;
// rejects 1D luts and malformed files. Returns false on any error.
bool ParseCube(std::istream& in, CubeLut* out) {
  std::string line;
  base::Vector<f32> data;
  u32 size = 0;
  while (std::getline(in, line)) {
    size_t s = line.find_first_not_of(" \t\r\n");
    if (s == std::string::npos || line[s] == '#') continue;
    std::istringstream ls(line.substr(s));
    std::string tok;
    ls >> tok;
    if (tok == "LUT_3D_SIZE") {
      ls >> size;
      if (size < 2 || size > 128) return false;
    } else if (tok == "LUT_1D_SIZE") {
      return false;  // 1D luts are not supported by the strip path
    } else if (tok == "TITLE" || tok == "DOMAIN_MIN" || tok == "DOMAIN_MAX" ||
               tok == "LUT_3D_INPUT_RANGE") {
      continue;
    } else {
      f32 r, g, b;
      std::istringstream vs(line.substr(s));
      if (!(vs >> r >> g >> b)) continue;  // tolerate stray lines
      data.push_back(r);
      data.push_back(g);
      data.push_back(b);
    }
  }
  if (size == 0 || data.size() != static_cast<size_t>(size) * size * size * 3) return false;
  out->size = size;
  out->rgb = std::move(data);
  return true;
}

// Trilinear sample of the cube at continuous voxel coords (red fastest layout).
void SampleCube(const CubeLut& lut, f32 fr, f32 fg, f32 fb, f32 out[3]) {
  const i32 n = static_cast<i32>(lut.size);
  auto fetch = [&](i32 r, i32 g, i32 b, i32 c) -> f32 {
    r = r < 0 ? 0 : (r >= n ? n - 1 : r);
    g = g < 0 ? 0 : (g >= n ? n - 1 : g);
    b = b < 0 ? 0 : (b >= n ? n - 1 : b);
    return lut.rgb[(static_cast<size_t>(b) * n * n + static_cast<size_t>(g) * n + r) * 3 + c];
  };
  i32 r0 = static_cast<i32>(fr), g0 = static_cast<i32>(fg), b0 = static_cast<i32>(fb);
  f32 dr = fr - r0, dg = fg - g0, db = fb - b0;
  for (i32 c = 0; c < 3; ++c) {
    f32 c00 = fetch(r0, g0, b0, c) * (1 - dr) + fetch(r0 + 1, g0, b0, c) * dr;
    f32 c10 = fetch(r0, g0 + 1, b0, c) * (1 - dr) + fetch(r0 + 1, g0 + 1, b0, c) * dr;
    f32 c01 = fetch(r0, g0, b0 + 1, c) * (1 - dr) + fetch(r0 + 1, g0, b0 + 1, c) * dr;
    f32 c11 = fetch(r0, g0 + 1, b0 + 1, c) * (1 - dr) + fetch(r0 + 1, g0 + 1, b0 + 1, c) * dr;
    f32 c0 = c00 * (1 - dg) + c10 * dg;
    f32 c1 = c01 * (1 - dg) + c11 * dg;
    out[c] = Clamp01(c0 * (1 - db) + c1 * db);
  }
}

}  // namespace

bool PostPass::CreateLut() {
  const u32 width = kLutSize * kLutSize;
  lut_ = device_.CreateImage2D(Format::kRGBA8Unorm, {width, kLutSize},
                               kTextureUsageSampled | kTextureUsageTransferDst);
  if (!lut_) return false;
  UploadLut(ColorGrade::kNeutral);
  return true;
}

void PostPass::UploadLut(ColorGrade grade) {
  const u32 size = kLutSize;
  const u32 width = size * size;
  base::Vector<u8> pixels(static_cast<size_t>(width) * size * 4);
  for (u32 b = 0; b < size; ++b) {
    for (u32 g = 0; g < size; ++g) {
      for (u32 r = 0; r < size; ++r) {
        f32 in[3] = {static_cast<f32>(r) / (size - 1), static_cast<f32>(g) / (size - 1),
                     static_cast<f32>(b) / (size - 1)};
        f32 out[3];
        ApplyGrade(grade, in, out);
        u8* p = &pixels[(static_cast<size_t>(g) * width + (b * size + r)) * 4];
        p[0] = static_cast<u8>(out[0] * 255.0f + 0.5f);
        p[1] = static_cast<u8>(out[1] * 255.0f + 0.5f);
        p[2] = static_cast<u8>(out[2] * 255.0f + 0.5f);
        p[3] = 255;
      }
    }
  }
  UploadLutPixels(pixels);
  lut_grade_ = grade;
}

void PostPass::UploadLutPixels(base::Vector<u8>& pixels) {
  const u32 size = kLutSize;
  const u32 width = size * size;
  GpuBuffer staging =
      device_.CreateBufferWithData({pixels.data(), pixels.size()}, kBufferUsageTransferSrc);
  bool first = !lut_ready_;
  device_.ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(
        lut_, first ? ResourceState::kUndefined : ResourceState::kShaderReadFragment,
        ResourceState::kCopyDst));

    BufferTextureCopy copy{.extent = {width, size}};
    cmd.CopyBufferToTexture(staging, lut_, {&copy, 1});

    cmd.Barrier(Transition(lut_, ResourceState::kCopyDst, ResourceState::kShaderReadFragment));
  });
  device_.DestroyBuffer(staging);
  lut_ready_ = true;
}

bool PostPass::LoadCubeLut(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    REC_WARN("cube lut: cannot open {}", path);
    return false;
  }
  CubeLut cube;
  if (!ParseCube(file, &cube)) {
    REC_WARN("cube lut: failed to parse {} (need a valid 3D .cube)", path);
    return false;
  }

  // Resample the cube into the engine's 32^3 strip (blue slices laid out across).
  const u32 size = kLutSize;
  const u32 width = size * size;
  base::Vector<u8> pixels(static_cast<size_t>(width) * size * 4);
  f32 scale = static_cast<f32>(cube.size - 1) / static_cast<f32>(size - 1);
  for (u32 b = 0; b < size; ++b) {
    for (u32 g = 0; g < size; ++g) {
      for (u32 r = 0; r < size; ++r) {
        f32 out[3];
        SampleCube(cube, r * scale, g * scale, b * scale, out);
        u8* p = &pixels[(static_cast<size_t>(g) * width + (b * size + r)) * 4];
        p[0] = static_cast<u8>(out[0] * 255.0f + 0.5f);
        p[1] = static_cast<u8>(out[1] * 255.0f + 0.5f);
        p[2] = static_cast<u8>(out[2] * 255.0f + 0.5f);
        p[3] = 255;
      }
    }
  }

  if (lut_ready_) device_.WaitIdle();  // shared across frames; drain before reupload
  UploadLutPixels(pixels);
  lut_grade_ = ColorGrade::kCustom;
  REC_INFO("cube lut: loaded {} ({}^3)", path, cube.size);
  return true;
}

void PostPass::SetGrade(ColorGrade grade) {
  if (grade == lut_grade_) return;
  device_.WaitIdle();  // the lut is shared across frames; drain before reupload
  UploadLut(grade);
}

PostPass::~PostPass() {
  device_.DestroyPipeline(pipeline_);
  device_.DestroyImage(lut_);
}

void PostPass::Record(PassContext& ctx, TextureView input, TextureView bloom,
                      const GpuBuffer& exposure, u64 exposure_size, TextureView output,
                      Extent2D output_extent, const Params& params) {
  ColorAttachment color{.view = output, .load = LoadOp::kDontCare,  // fully overwritten
                        .store = StoreOp::kStore};
  ctx.cmd->BeginRendering({.extent = output_extent, .colors = {&color, 1}});
  ctx.cmd->BindPipeline(pipeline_);
  ctx.cmd->BindTransient(0, {Bind::Combined(0, input, sampler_),
                             Bind::Combined(1, bloom, sampler_),
                             Bind::StorageBuffer(2, exposure, 0, exposure_size),
                             Bind::Combined(3, lut_.view, sampler_)});
  ctx.cmd->Push(params);
  ctx.cmd->Draw(3);
  ctx.cmd->EndRendering();
}

}  // namespace rec::render
