#ifndef RECREATION_RENDER_RAYTRACING_H_
#define RECREATION_RENDER_RAYTRACING_H_

#include <memory>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/math.h"
#include "core/types.h"
#include "render/rhi/resources.h"

namespace rec::render {

class Device;

struct RayTracingSettings {
  bool shadows = true;
  bool reflections = false;
  bool global_illumination = false;
};

// Owns acceleration structures. Only constructed when DeviceCaps::raytracing
// is true; every effect it feeds degrades to the raster path otherwise.
// BLASes build once per uploaded mesh, the TLAS rebuilds every frame from
// the visible instances. Two TLAS slots ping pong so a rebuild never races
// the frame still in flight.
class RayTracingContext {
 public:
  static constexpr u32 kSlots = 2;

  struct Instance {
    u64 mesh_key = 0;
    u32 custom_index = 0;  // shader-visible instanceCustomIndex (bindless mesh record)
    Mat4 transform = Mat4::Identity();
  };

  static std::unique_ptr<RayTracingContext> Create(Device& device);
  ~RayTracingContext();

  RayTracingContext(const RayTracingContext&) = delete;
  RayTracingContext& operator=(const RayTracingContext&) = delete;

  void Configure(const RayTracingSettings& settings) { settings_ = settings; }
  const RayTracingSettings& settings() const { return settings_; }

  // Builds a BLAS for an uploaded mesh, keyed like the renderer's mesh map.
  // The mesh buffers must have been created with acceleration structure
  // build input usage.
  bool BuildBlas(u64 mesh_key, const GpuMesh& mesh);

  // Whether a blas already exists for this mesh (BuildBlas is idempotent, but
  // callers re-registering bindless geometry need to skip already-built meshes).
  bool HasBlas(u64 mesh_key) const { return blas_.contains(mesh_key); }

  // Grows the slot's TLAS to hold at least instance_count instances. This can
  // stall (device idle) and reallocate, so it MUST be called during the CPU
  // frame-build phase, never while a command buffer is recording. Doing the
  // growth here keeps BuildTlas allocation-free at record time.
  void ReserveTlas(u32 slot, u32 instance_count);

  // Records a full TLAS rebuild into cmd, including the barrier that makes
  // it visible to shader ray queries. Instances without a BLAS are skipped.
  // Capacity must already cover the instances (see ReserveTlas).
  void BuildTlas(VkCommandBuffer cmd, u32 slot, const base::Vector<Instance>& instances);

  VkAccelerationStructureKHR tlas(u32 slot) const { return tlas_[slot].handle; }

 private:
  struct Blas {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    GpuBuffer buffer;
    VkDeviceAddress address = 0;
  };

  struct Tlas {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    GpuBuffer buffer;
    GpuBuffer instances;  // host visible VkAccelerationStructureInstanceKHR
    GpuBuffer scratch;
    u32 capacity = 0;
  };

  explicit RayTracingContext(Device& device) : device_(device) {}

  bool EnsureTlasCapacity(Tlas& tlas, u32 instance_count);
  bool EnsureBlasScratch(u64 size);
  void DestroyTlas(Tlas& tlas);

  Device& device_;
  RayTracingSettings settings_;
  u32 scratch_alignment_ = 128;
  base::UnorderedMap<u64, Blas> blas_;
  // Reused across builds. Freeing scratch right after the fence tripped
  // lavapipe, whose build workers can outlive the signal; a persistent
  // arena avoids both that and the per-build allocation.
  GpuBuffer blas_scratch_;
  Tlas tlas_[kSlots];
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_RAYTRACING_H_
