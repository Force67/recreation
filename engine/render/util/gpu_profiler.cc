#include "render/util/gpu_profiler.h"

#include "render/rhi/device.h"

namespace rec::render {

bool GpuProfiler::Initialize(Device& device, u32 frames_in_flight) {
  if (device.is_stub()) return false;
  f32 period = device.caps().timestamp_period;
  if (period <= 0.0f) return false;  // no timestamp support, profiler stays off

  period_ns_ = period;
  frames_.clear();
  for (u32 i = 0; i < frames_in_flight; ++i) {
    VkQueryPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kQueriesPerFrame;
    FramePool fp;
    if (vkCreateQueryPool(device.device(), &info, nullptr, &fp.pool) != VK_SUCCESS) {
      return false;
    }
    frames_.push_back(std::move(fp));
  }
  device_ = &device;
  return true;
}

void GpuProfiler::Shutdown() {
  if (!device_) return;
  for (FramePool& fp : frames_) {
    if (fp.pool) vkDestroyQueryPool(device_->device(), fp.pool, nullptr);
  }
  frames_.clear();
  device_ = nullptr;
}

void GpuProfiler::BeginFrame(VkCommandBuffer cmd, u32 frame_slot) {
  if (!device_ || frames_.empty()) return;
  current_ = frame_slot % static_cast<u32>(frames_.size());
  FramePool& fp = frames_[current_];

  // The fence for this slot already fired, so the timestamps from its previous
  // use are readable. Convert pairs to milliseconds.
  if (fp.recorded && fp.pass_count > 0) {
    base::Vector<u64> stamps(fp.pass_count * 2);
    VkResult r = vkGetQueryPoolResults(
        device_->device(), fp.pool, 0, fp.pass_count * 2, stamps.size() * sizeof(u64),
        stamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (r == VK_SUCCESS) {
      results_.clear();
      total_ms_ = 0.0f;
      for (u32 i = 0; i < fp.pass_count; ++i) {
        u64 begin = stamps[i * 2];
        u64 end = stamps[i * 2 + 1];
        f32 ms = end > begin ? static_cast<f32>((end - begin) * period_ns_) * 1e-6f : 0.0f;
        results_.push_back({fp.names[i], ms});
        total_ms_ += ms;
      }
    }
  }

  vkCmdResetQueryPool(cmd, fp.pool, 0, kQueriesPerFrame);
  fp.names.clear();
  fp.pass_count = 0;
  fp.recorded = true;
}

void GpuProfiler::BeginPass(VkCommandBuffer cmd, const char* name) {
  if (!device_ || frames_.empty()) return;
  FramePool& fp = frames_[current_];
  if (device_->caps().debug_utils) {
    VkDebugUtilsLabelEXT label{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    label.color[0] = 0.4f;
    label.color[1] = 0.7f;
    label.color[2] = 1.0f;
    label.color[3] = 1.0f;
    vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
  }
  if (fp.pass_count >= kMaxPasses) return;
  fp.names.push_back(name);
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, fp.pool, fp.pass_count * 2);
}

void GpuProfiler::EndPass(VkCommandBuffer cmd) {
  if (!device_ || frames_.empty()) return;
  FramePool& fp = frames_[current_];
  if (fp.pass_count < kMaxPasses) {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fp.pool, fp.pass_count * 2 + 1);
    ++fp.pass_count;
  }
  if (device_->caps().debug_utils) vkCmdEndDebugUtilsLabelEXT(cmd);
}

}  // namespace rec::render
