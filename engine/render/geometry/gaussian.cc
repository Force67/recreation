#include "render/geometry/gaussian.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "core/log.h"
#include "shaders/gsplat_ps_hlsl.h"
#include "shaders/gsplat_vs_hlsl.h"

namespace rec::render {
namespace {

struct GaussianPush {
  Mat4 view;
  f32 proj_x;
  f32 proj_y;
  f32 near_plane;
  f32 screen_x;
  f32 screen_y;
  f32 pad[3];
};

u32 PlyTypeSize(const std::string& t) {
  if (t == "char" || t == "uchar" || t == "int8" || t == "uint8") return 1;
  if (t == "short" || t == "ushort" || t == "int16" || t == "uint16") return 2;
  if (t == "int" || t == "uint" || t == "int32" || t == "uint32" || t == "float" || t == "float32")
    return 4;
  if (t == "double" || t == "float64") return 8;
  return 0;
}

f32 Sigmoid(f32 x) { return 1.0f / (1.0f + std::exp(-x)); }

}  // namespace

bool GaussianSplat::Initialize(Device& device, Format color_format) {
  // TODO(rhi): blend preset mismatch: old alpha factors were ZERO/ONE (dst alpha
  // preserved); kAlpha uses ONE/ONE_MINUS_SRC_ALPHA.
  pipeline_ = device.CreateGraphicsPipeline({
      .vertex = REC_SHADER(k_gsplat_vs_hlsl),
      .fragment = REC_SHADER(k_gsplat_ps_hlsl),
      .topology = PrimitiveTopology::kTriangleStrip,
      .raster = {.cull = CullMode::kNone},
      .color_formats = {color_format},
      .blend = {BlendMode::kAlpha},
      .sets = {{.slots = {{0, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(GaussianPush),
      .debug_name = "gaussian_splat",
  });
  if (!pipeline_) {
    REC_ERROR("gaussian pipeline creation failed");
    return false;
  }

  for (u32 i = 0; i < kFramesInFlight; ++i) {
    buffers_[i] = device.CreateBuffer(static_cast<u64>(kMaxGaussians) * sizeof(GaussianInstance),
                                      kBufferUsageStorage, true);
    if (!buffers_[i].mapped) return false;
  }
  return true;
}

void GaussianSplat::AddToGraph(RenderGraph& graph, ResourceHandle color,
                               const base::Vector<GaussianInstance>& gaussians, const Frame& frame,
                               u32 frame_slot) {
  if (gaussians.empty()) return;
  u32 count = std::min(static_cast<u32>(gaussians.size()), kMaxGaussians);

  // Sort back-to-front by view depth (front = -z, so most-negative first).
  base::Vector<u32> order(count);
  for (u32 i = 0; i < count; ++i) order[i] = i;
  const Mat4& v = frame.view;
  auto view_z = [&](u32 i) {
    const GaussianInstance& g = gaussians[i];
    return v.m[2] * g.position[0] + v.m[6] * g.position[1] + v.m[10] * g.position[2] + v.m[14];
  };
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return view_z(a) < view_z(b); });

  GaussianInstance* dst = static_cast<GaussianInstance*>(buffers_[frame_slot].mapped);
  for (u32 i = 0; i < count; ++i) dst[i] = gaussians[order[i]];
  GpuBuffer buffer = buffers_[frame_slot];

  graph.AddPass(
      "gaussian_splat",
      [&](RenderGraph::PassBuilder& builder) { builder.Write(color, ResourceUsage::kColorAttachment); },
      [this, color, buffer, count, frame](PassContext& ctx) {
        const GpuImage& target = ctx.graph->image(color);
        ColorAttachment attachment[] = {{.view = target.view, .load = LoadOp::kLoad}};
        ctx.cmd->BeginRendering({.extent = target.extent, .colors = attachment});

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::StorageBuffer(0, buffer, 0, count * sizeof(GaussianInstance))});

        GaussianPush push{};
        push.view = frame.view;
        push.proj_x = frame.proj_x;
        push.proj_y = frame.proj_y;
        push.near_plane = frame.near_plane;
        push.screen_x = frame.screen_x;
        push.screen_y = frame.screen_y;
        ctx.cmd->Push(push);
        ctx.cmd->Draw(4, count, 0, 0);
        ctx.cmd->EndRendering();
      });
}

void GaussianSplat::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  for (u32 i = 0; i < kFramesInFlight; ++i) device.DestroyBuffer(buffers_[i]);
}

bool LoadGaussianPly(const std::string& path, base::Vector<GaussianInstance>* out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    REC_WARN("gaussian ply: cannot open {}", path);
    return false;
  }
  std::string line;
  if (!std::getline(file, line) || line.compare(0, 3, "ply") != 0) {
    REC_WARN("gaussian ply: {} is not a ply file", path);
    return false;
  }

  struct Prop {
    std::string name;
    u32 size = 0;
    u32 offset = 0;
  };
  struct Elem {
    std::string name;
    u64 count = 0;
    base::Vector<Prop> props;
    u32 stride = 0;
    bool has_list = false;
  };
  base::Vector<Elem> elems;
  bool binary = false, little = true;
  while (std::getline(file, line)) {
    std::istringstream ls(line);
    std::string tok;
    ls >> tok;
    if (tok == "end_header") break;
    if (tok == "format") {
      std::string fmt;
      ls >> fmt;
      binary = fmt.compare(0, 6, "binary") == 0;
      little = fmt != "binary_big_endian";
    } else if (tok == "element") {
      Elem e;
      ls >> e.name >> e.count;
      elems.push_back(std::move(e));
    } else if (tok == "property" && !elems.empty()) {
      std::string type;
      ls >> type;
      Elem& e = elems.back();
      if (type == "list") {  // face index lists; not present on splat vertices
        std::string a, b, nm;
        ls >> a >> b >> nm;
        e.props.push_back({nm, 0, 0});
        e.has_list = true;
      } else {
        std::string nm;
        ls >> nm;
        u32 sz = PlyTypeSize(type);
        e.props.push_back({nm, sz, e.stride});
        e.stride += sz;
      }
    }
  }
  if (binary && !little) {
    REC_WARN("gaussian ply: big-endian binary is not supported");
    return false;
  }

  const f32 kC0 = 0.28209479177387814f;  // sh band 0 constant
  const u64 kCap = 1u << 18;             // matches the renderer's gaussian budget

  for (Elem& e : elems) {
    if (e.name != "vertex") {  // skip other elements (e.g. faces) to reach vertex
      if (binary) {
        if (e.has_list) {
          REC_WARN("gaussian ply: variable-size element before vertex is unsupported");
          return false;
        }
        file.seekg(static_cast<std::streamoff>(e.count * e.stride), std::ios::cur);
      } else {
        for (u64 i = 0; i < e.count && std::getline(file, line); ++i) {
        }
      }
      continue;
    }

    auto index_of = [&](const char* nm) -> int {
      for (size_t i = 0; i < e.props.size(); ++i)
        if (e.props[i].name == nm) return static_cast<int>(i);
      return -1;
    };
    int ix = index_of("x"), iy = index_of("y"), iz = index_of("z");
    if (ix < 0 || iy < 0 || iz < 0) {
      REC_WARN("gaussian ply: vertex element has no x/y/z");
      return false;
    }
    int iop = index_of("opacity");
    int is0 = index_of("scale_0"), is1 = index_of("scale_1"), is2 = index_of("scale_2");
    int ir0 = index_of("rot_0"), ir1 = index_of("rot_1"), ir2 = index_of("rot_2"),
        ir3 = index_of("rot_3");
    int if0 = index_of("f_dc_0"), if1 = index_of("f_dc_1"), if2 = index_of("f_dc_2");

    auto decode = [](const u8* rec, const Prop& p) -> f32 {
      if (p.size == 4) {
        float v;
        std::memcpy(&v, rec + p.offset, 4);
        return v;
      }
      if (p.size == 8) {
        double v;
        std::memcpy(&v, rec + p.offset, 8);
        return static_cast<f32>(v);
      }
      if (p.size == 2) {
        u16 v;
        std::memcpy(&v, rec + p.offset, 2);
        return static_cast<f32>(v);
      }
      if (p.size == 1) return static_cast<f32>(rec[p.offset]);
      return 0.0f;
    };

    base::Vector<u8> rec(e.stride);
    base::Vector<f32> vals(e.props.size());  // every entry is rewritten per record
    bool truncated = false;
    for (u64 i = 0; i < e.count; ++i) {
      if (binary) {
        file.read(reinterpret_cast<char*>(rec.data()), e.stride);
        if (!file) break;
        for (size_t k = 0; k < e.props.size(); ++k) vals[k] = decode(rec.data(), e.props[k]);
      } else {
        if (!std::getline(file, line)) break;
        std::istringstream vs(line);
        for (size_t k = 0; k < e.props.size(); ++k) {
          f32 v = 0.0f;
          vs >> v;
          vals[k] = v;
        }
      }
      if (out->size() >= kCap) {
        truncated = true;
        break;
      }
      auto at = [&](int j) { return j >= 0 ? vals[static_cast<size_t>(j)] : 0.0f; };
      GaussianInstance g;  // defaults cover any missing properties
      g.position[0] = at(ix);
      g.position[1] = at(iy);
      g.position[2] = at(iz);
      if (iop >= 0) g.opacity = Sigmoid(at(iop));
      if (is0 >= 0 && is1 >= 0 && is2 >= 0) {
        g.scale[0] = std::exp(at(is0));
        g.scale[1] = std::exp(at(is1));
        g.scale[2] = std::exp(at(is2));
      }
      if (if0 >= 0 && if1 >= 0 && if2 >= 0) {
        g.color[0] = std::clamp(0.5f + kC0 * at(if0), 0.0f, 1.0f);
        g.color[1] = std::clamp(0.5f + kC0 * at(if1), 0.0f, 1.0f);
        g.color[2] = std::clamp(0.5f + kC0 * at(if2), 0.0f, 1.0f);
      }
      if (ir0 >= 0 && ir1 >= 0 && ir2 >= 0 && ir3 >= 0) {
        f32 w = at(ir0), x = at(ir1), y = at(ir2), z = at(ir3);  // inria stores wxyz
        f32 len = std::sqrt(w * w + x * x + y * y + z * z);
        if (len < 1e-8f) len = 1.0f;
        g.rotation[0] = x / len;
        g.rotation[1] = y / len;
        g.rotation[2] = z / len;
        g.rotation[3] = w / len;
      }
      out->push_back(g);
    }
    if (truncated) REC_WARN("gaussian ply: clamped to {} splats", kCap);
    break;  // vertex element handled; ignore anything after it
  }

  if (out->empty()) {
    REC_WARN("gaussian ply: no vertices read from {}", path);
    return false;
  }
  REC_INFO("gaussian ply: loaded {} splats from {}", out->size(), path);
  return true;
}

}  // namespace rec::render
