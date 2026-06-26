#include "render/gi/raytracing.h"

#include <algorithm>
#include <cstring>

#include "asset/mesh.h"
#include "core/log.h"
#include "render/rhi/device.h"

namespace rec::render {
namespace {

VkDeviceAddress BufferAddress(VkDevice device, VkBuffer buffer) {
  VkBufferDeviceAddressInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = buffer;
  return vkGetBufferDeviceAddress(device, &info);
}

VkDeviceAddress AlignUp(VkDeviceAddress address, VkDeviceAddress alignment) {
  return (address + alignment - 1) & ~(alignment - 1);
}

VkTransformMatrixKHR ToTransformMatrix(const Mat4& m) {
  // VkTransformMatrixKHR is row major 3x4, Mat4 is column major.
  VkTransformMatrixKHR result;
  for (u32 row = 0; row < 3; ++row) {
    for (u32 col = 0; col < 4; ++col) {
      result.matrix[row][col] = m.m[col * 4 + row];
    }
  }
  return result;
}

}  // namespace

std::unique_ptr<RayTracingContext> RayTracingContext::Create(Device& device) {
  auto context = std::unique_ptr<RayTracingContext>(new RayTracingContext(device));

  VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
  VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  props.pNext = &as_props;
  vkGetPhysicalDeviceProperties2(device.physical_device(), &props);
  context->scratch_alignment_ = as_props.minAccelerationStructureScratchOffsetAlignment;
  return context;
}

RayTracingContext::~RayTracingContext() {
  for (auto kv : blas_) {
    vkDestroyAccelerationStructureKHR(device_.device(), kv.value.handle, nullptr);
    device_.DestroyBuffer(kv.value.buffer);
  }
  device_.DestroyBuffer(blas_scratch_);
  for (Tlas& tlas : tlas_) DestroyTlas(tlas);
}

bool RayTracingContext::EnsureBlasScratch(u64 size) {
  if (blas_scratch_.buffer && blas_scratch_.size >= size) return true;
  device_.WaitIdle();
  device_.DestroyBuffer(blas_scratch_);
  blas_scratch_ = device_.CreateBuffer(
      size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  return blas_scratch_.buffer != VK_NULL_HANDLE;
}

void RayTracingContext::DestroyTlas(Tlas& tlas) {
  if (tlas.handle) vkDestroyAccelerationStructureKHR(device_.device(), tlas.handle, nullptr);
  device_.DestroyBuffer(tlas.buffer);
  device_.DestroyBuffer(tlas.instances);
  device_.DestroyBuffer(tlas.scratch);
  tlas = {};
}

bool RayTracingContext::BuildBlas(u64 mesh_key, const GpuMesh& mesh) {
  if (blas_.contains(mesh_key)) return true;
  if (mesh.vertex_count == 0 || mesh.index_count == 0) return false;

  // One geometry per opaque submesh: hit shaders resolve the material from
  // CommittedGeometryIndex through the bindless geometry table, which is
  // written in the same order. Blend submeshes stay out entirely.
  VkAccelerationStructureGeometryKHR geometry_template{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geometry_template.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geometry_template.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  auto& triangles = geometry_template.geometry.triangles;
  triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
  triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  triangles.vertexData.deviceAddress = BufferAddress(device_.device(), mesh.vertices.buffer);
  triangles.vertexStride = sizeof(asset::Vertex);
  triangles.maxVertex = mesh.vertex_count - 1;
  triangles.indexType = VK_INDEX_TYPE_UINT32;
  triangles.indexData.deviceAddress = BufferAddress(device_.device(), mesh.indices.buffer);

  base::Vector<VkAccelerationStructureGeometryKHR> geometries;
  base::Vector<VkAccelerationStructureBuildRangeInfoKHR> range_infos;
  base::Vector<u32> primitive_counts;
  for (const GpuSubmesh& submesh : mesh.submeshes) {
    if (submesh.blend || submesh.index_count == 0) continue;
    geometries.push_back(geometry_template);
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = submesh.index_count / 3;
    range.primitiveOffset = submesh.index_offset * sizeof(u32);
    range_infos.push_back(range);
    primitive_counts.push_back(range.primitiveCount);
  }
  if (geometries.empty()) return false;

  VkAccelerationStructureBuildGeometryInfoKHR build{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = static_cast<u32>(geometries.size());
  build.pGeometries = geometries.data();

  VkAccelerationStructureBuildSizesInfoKHR sizes{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device_.device(),
                                          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                                          primitive_counts.data(), &sizes);

  Blas blas;
  blas.buffer = device_.CreateBuffer(
      sizes.accelerationStructureSize,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  if (!blas.buffer.buffer) return false;

  VkAccelerationStructureCreateInfoKHR create_info{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  create_info.buffer = blas.buffer.buffer;
  create_info.size = sizes.accelerationStructureSize;
  create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  if (vkCreateAccelerationStructureKHR(device_.device(), &create_info, nullptr, &blas.handle) !=
      VK_SUCCESS) {
    device_.DestroyBuffer(blas.buffer);
    return false;
  }

  if (!EnsureBlasScratch(sizes.buildScratchSize + scratch_alignment_)) {
    vkDestroyAccelerationStructureKHR(device_.device(), blas.handle, nullptr);
    device_.DestroyBuffer(blas.buffer);
    return false;
  }
  build.dstAccelerationStructure = blas.handle;
  build.scratchData.deviceAddress =
      AlignUp(BufferAddress(device_.device(), blas_scratch_.buffer), scratch_alignment_);

  const VkAccelerationStructureBuildRangeInfoKHR* ranges = range_infos.data();
  device_.ImmediateSubmit([&](VkCommandBuffer cmd) {
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
  });

  VkAccelerationStructureDeviceAddressInfoKHR address_info{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  address_info.accelerationStructure = blas.handle;
  blas.address = vkGetAccelerationStructureDeviceAddressKHR(device_.device(), &address_info);

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

  tlas.instances = device_.CreateBuffer(
      capacity * sizeof(VkAccelerationStructureInstanceKHR),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      true);
  if (!tlas.instances.mapped) return false;

  VkAccelerationStructureGeometryKHR geometry{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
  geometry.geometry.instances.data.deviceAddress =
      BufferAddress(device_.device(), tlas.instances.buffer);

  VkAccelerationStructureBuildGeometryInfoKHR build{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;

  VkAccelerationStructureBuildSizesInfoKHR sizes{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device_.device(),
                                          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                                          &capacity, &sizes);

  tlas.buffer = device_.CreateBuffer(
      sizes.accelerationStructureSize,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  tlas.scratch = device_.CreateBuffer(
      sizes.buildScratchSize + scratch_alignment_,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  if (!tlas.buffer.buffer || !tlas.scratch.buffer) return false;

  VkAccelerationStructureCreateInfoKHR create_info{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  create_info.buffer = tlas.buffer.buffer;
  create_info.size = sizes.accelerationStructureSize;
  create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  if (vkCreateAccelerationStructureKHR(device_.device(), &create_info, nullptr, &tlas.handle) !=
      VK_SUCCESS) {
    return false;
  }
  tlas.capacity = capacity;
  return true;
}

void RayTracingContext::ReserveTlas(u32 slot, u32 instance_count) {
  // Reserve for the upper bound (some instances may lack a BLAS and drop out in
  // BuildTlas, but never more than this); a stall/realloc here is safe.
  EnsureTlasCapacity(tlas_[slot], std::max(instance_count, 1u));
}

void RayTracingContext::BuildTlas(VkCommandBuffer cmd, u32 slot,
                                  const base::Vector<Instance>& instances) {
  Tlas& tlas = tlas_[slot];

  base::Vector<VkAccelerationStructureInstanceKHR> gpu_instances;
  gpu_instances.reserve(instances.size());
  for (const Instance& instance : instances) {
    const Blas* blas = blas_.find(instance.mesh_key);
    if (!blas) continue;
    VkAccelerationStructureInstanceKHR gpu{};
    gpu.transform = ToTransformMatrix(instance.transform);
    gpu.instanceCustomIndex = instance.custom_index & 0xffffffu;
    gpu.mask = 0xff;
    gpu.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    gpu.accelerationStructureReference = blas->address;
    gpu_instances.push_back(gpu);
  }

  u32 count = static_cast<u32>(gpu_instances.size());
  if (!EnsureTlasCapacity(tlas, std::max(count, 1u))) {
    REC_ERROR("tlas allocation failed");
    return;
  }
  if (count > 0) {
    std::memcpy(tlas.instances.mapped, gpu_instances.data(),
                count * sizeof(VkAccelerationStructureInstanceKHR));
  }

  VkAccelerationStructureGeometryKHR geometry{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
  geometry.geometry.instances.data.deviceAddress =
      BufferAddress(device_.device(), tlas.instances.buffer);

  VkAccelerationStructureBuildGeometryInfoKHR build{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;
  build.dstAccelerationStructure = tlas.handle;
  build.scratchData.deviceAddress =
      AlignUp(BufferAddress(device_.device(), tlas.scratch.buffer), scratch_alignment_);

  VkAccelerationStructureBuildRangeInfoKHR range{.primitiveCount = count};
  const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
  vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);

  VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  barrier.dstStageMask =
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace rec::render
