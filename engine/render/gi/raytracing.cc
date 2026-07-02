#include "render/gi/raytracing.h"

#include <algorithm>
#include <cstring>

#include "asset/mesh.h"
#include "core/log.h"
#include "render/rhi/device.h"

namespace rec::render {
namespace {

u64 AlignUp(u64 value, u64 alignment) { return (value + alignment - 1) & ~(alignment - 1); }

void ToInstanceTransform(const Mat4& m, f32 out[3][4]) {
  // TlasInstance transforms are row major 3x4, Mat4 is column major.
  for (u32 row = 0; row < 3; ++row) {
    for (u32 col = 0; col < 4; ++col) {
      out[row][col] = m.m[col * 4 + row];
    }
  }
}

// One geometry per opaque submesh: hit shaders resolve the material from
// CommittedGeometryIndex through the bindless geometry table, which is
// written in the same order. Blend submeshes stay out entirely. Alpha-masked
// (cutout) submeshes go in non-opaque so a ray query can alpha-test them; the
// realtime paths trace RAY_FLAG_FORCE_OPAQUE which overrides that to opaque.
base::Vector<AccelTriangles> BlasGeometries(const GpuMesh& mesh) {
  base::Vector<AccelTriangles> geometries;
  for (const GpuSubmesh& submesh : mesh.submeshes) {
    if (submesh.blend || submesh.index_count == 0) continue;
    geometries.push_back({.vertex_address = mesh.vertices.address,
                          .vertex_stride = sizeof(asset::Vertex),
                          .vertex_count = mesh.vertex_count,
                          .vertex_format = Format::kRGB32Float,
                          .index_address = mesh.indices.address + submesh.index_offset * sizeof(u32),
                          .index_count = submesh.index_count,
                          .index_type = IndexType::kUint32,
                          .opaque = !submesh.alpha_mask});
  }
  return geometries;
}

}  // namespace

std::unique_ptr<RayTracingContext> RayTracingContext::Create(Device& device) {
  return std::unique_ptr<RayTracingContext>(new RayTracingContext(device));
}

RayTracingContext::~RayTracingContext() {
  for (auto kv : blas_) device_.DestroyAccelStruct(kv.value.handle);
  device_.DestroyBuffer(blas_scratch_);
  for (Tlas& tlas : tlas_) DestroyTlas(tlas);
}

bool RayTracingContext::EnsureBlasScratch(u64 size) {
  if (blas_scratch_ && blas_scratch_.size >= size) return true;
  device_.WaitIdle();
  device_.DestroyBuffer(blas_scratch_);
  blas_scratch_ = device_.CreateBuffer(size, kBufferUsageAccelScratch);
  return static_cast<bool>(blas_scratch_);
}

void RayTracingContext::DestroyTlas(Tlas& tlas) {
  if (tlas.handle) device_.DestroyAccelStruct(tlas.handle);
  device_.DestroyBuffer(tlas.instances);
  device_.DestroyBuffer(tlas.scratch);
  tlas = {};
}

bool RayTracingContext::BuildBlas(u64 mesh_key, const GpuMesh& mesh) {
  if (blas_.contains(mesh_key)) return true;
  if (mesh.vertex_count == 0 || mesh.index_count == 0) return false;
  if (mesh.vertices.address == 0 || mesh.indices.address == 0) return false;

  base::Vector<AccelTriangles> geometries = BlasGeometries(mesh);
  if (geometries.empty()) return false;

  BlasBuildDesc desc{.geometries = {geometries.data(), geometries.size()}};
  AccelSizes sizes = device_.GetBlasSizes(desc);
  if (sizes.accel_bytes == 0) return false;

  Blas blas;
  blas.handle = device_.CreateAccelStruct(AccelStructType::kBlas, sizes.accel_bytes);
  if (!blas.handle) return false;

  u32 alignment = device_.caps().accel_scratch_alignment;
  if (!EnsureBlasScratch(sizes.scratch_bytes + alignment)) {
    device_.DestroyAccelStruct(blas.handle);
    return false;
  }
  u64 scratch_offset = AlignUp(blas_scratch_.address, alignment) - blas_scratch_.address;

  device_.ImmediateSubmit([&](CommandList& cmd) {
    cmd.BuildBlas(blas.handle, desc, blas_scratch_, scratch_offset);
  });
  blas.address = device_.accel_address(blas.handle);

  blas_.emplace(mesh_key, blas);
  return true;
}

bool RayTracingContext::EnsureTlasCapacity(Tlas& tlas, u32 instance_count) {
  if (tlas.handle && tlas.capacity >= instance_count) return true;

  // Growth recreates everything. Rare in practice (entity count spikes),
  // so a full stall beats juggling retired buffers.
  device_.WaitIdle();
  DestroyTlas(tlas);

  u32 capacity = 64;
  while (capacity < instance_count) capacity *= 2;

  tlas.instances = device_.CreateBuffer(capacity * sizeof(TlasInstance),
                                        kBufferUsageAccelBuildInput, true);
  if (!tlas.instances.mapped) return false;

  AccelSizes sizes = device_.GetTlasSizes(capacity);
  tlas.handle = device_.CreateAccelStruct(AccelStructType::kTlas, sizes.accel_bytes);
  u32 alignment = device_.caps().accel_scratch_alignment;
  tlas.scratch = device_.CreateBuffer(sizes.scratch_bytes + alignment, kBufferUsageAccelScratch);
  if (!tlas.handle || !tlas.scratch) return false;
  tlas.capacity = capacity;
  return true;
}

void RayTracingContext::ReserveTlas(u32 slot, u32 instance_count) {
  // Reserve for the upper bound (some instances may lack a BLAS and drop out in
  // BuildTlas, but never more than this); a stall/realloc here is safe.
  EnsureTlasCapacity(tlas_[slot], std::max(instance_count, 1u));
}

void RayTracingContext::BuildTlas(CommandList& cmd, u32 slot,
                                  const base::Vector<Instance>& instances) {
  Tlas& tlas = tlas_[slot];

  base::Vector<TlasInstance> gpu_instances;
  gpu_instances.reserve(instances.size());
  for (const Instance& instance : instances) {
    const Blas* blas = blas_.find(instance.mesh_key);
    if (!blas) continue;
    TlasInstance gpu{};
    ToInstanceTransform(instance.transform, gpu.transform);
    gpu.custom_index = instance.custom_index & 0xffffffu;
    gpu.mask = 0xff;
    gpu.flags = kTlasInstanceTriangleCullDisable;
    gpu.blas_address = blas->address;
    gpu_instances.push_back(gpu);
  }

  u32 count = static_cast<u32>(gpu_instances.size());
  if (!EnsureTlasCapacity(tlas, std::max(count, 1u))) {
    REC_ERROR("tlas allocation failed");
    return;
  }
  if (count > 0) {
    std::memcpy(tlas.instances.mapped, gpu_instances.data(), count * sizeof(TlasInstance));
  }

  cmd.BuildTlas(tlas.handle, tlas.instances, count, tlas.scratch);
  cmd.MemoryBarrier(BarrierScope::kAccelBuildWrite, BarrierScope::kAccelRead);
}

}  // namespace rec::render
