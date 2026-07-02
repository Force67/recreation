#ifndef RECREATION_RENDER_RHI_PIPELINE_H_
#define RECREATION_RENDER_RHI_PIPELINE_H_

#include <base/containers/vector.h>

#include "core/types.h"
#include "render/rhi/bindings.h"
#include "render/rhi/types.h"

namespace rec::render {

// A pipeline's descriptor-set interface. Most passes declare their slots
// inline and let the device derive (and cache) the layout; sets shared across
// pipelines (bindless registry, frame globals) pass the existing layout
// handle instead.
struct PipelineBindings {
  base::Vector<BindingSlot> slots;       // inline definition, or
  BindingLayoutHandle shared;            // an externally created layout
  ShaderStageFlags stages = kShaderStageNone;  // 0 = all stages of the pipeline
};

struct ComputePipelineDesc {
  ShaderBlob shader;
  base::Vector<PipelineBindings> sets;
  u32 push_constant_size = 0;
  const char* debug_name = nullptr;
};

struct VertexAttribute {
  u32 location = 0;
  Format format = Format::kRGB32Float;
  u32 offset = 0;
};

struct VertexBufferLayout {
  u32 stride = 0;
  bool per_instance = false;
  base::Vector<VertexAttribute> attributes;
};

struct DepthState {
  bool test = false;
  bool write = false;
  CompareOp compare = CompareOp::kGreaterEqual;  // reversed-z default
  Format format = Format::kUnknown;              // kUnknown = no depth attachment
  f32 bias_constant = 0.0f;
  f32 bias_slope = 0.0f;
};

struct RasterState {
  CullMode cull = CullMode::kBack;
  FrontFace front = FrontFace::kCounterClockwise;
  PolygonMode polygon = PolygonMode::kFill;
};

// Dynamic-rendering only: attachment formats are part of the pipeline, render
// targets bind at record time. Viewport/scissor are always dynamic.
struct GraphicsPipelineDesc {
  ShaderBlob vertex;    // exclusive with task/mesh
  ShaderBlob fragment;
  ShaderBlob task;      // mesh-shader path
  ShaderBlob mesh;
  base::Vector<VertexBufferLayout> vertex_buffers;
  PrimitiveTopology topology = PrimitiveTopology::kTriangleList;
  RasterState raster;
  DepthState depth;
  base::Vector<Format> color_formats;
  base::Vector<BlendMode> blend;  // per color target; empty = all opaque
  base::Vector<PipelineBindings> sets;
  u32 push_constant_size = 0;
  const char* debug_name = nullptr;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RHI_PIPELINE_H_
