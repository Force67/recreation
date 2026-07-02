#ifndef RECREATION_RENDER_GPU_PROFILER_H_
#define RECREATION_RENDER_GPU_PROFILER_H_

#include <string>

#include <base/containers/vector.h>

#include "core/types.h"
#include "render/rhi/types.h"

namespace rec::render {

class CommandList;
class Device;

// Per-pass GPU timing via timestamp queries plus debug labels so the same
// pass boundaries show up in capture tools. One query pool per frame in
// flight: a frame's results are read back the next time that slot is reused,
// which the in-flight fence already guarantees is complete.
class GpuProfiler {
 public:
  struct PassTiming {
    std::string name;
    f32 ms = 0.0f;
  };

  bool Initialize(Device& device, u32 frames_in_flight);
  void Shutdown();

  // Resolves the previous results recorded into this slot, then records a
  // query-pool reset into cmd. Call once right after frame recording begins.
  void BeginFrame(CommandList& cmd, u32 frame_slot);

  // Bracket a render-graph pass. BeginPass opens a debug label and writes a
  // top-of-pipe timestamp; EndPass writes bottom-of-pipe and closes the label.
  void BeginPass(CommandList& cmd, const char* name);
  void EndPass(CommandList& cmd);

  bool available() const { return device_ != nullptr; }
  // Last fully resolved frame's per-pass timings.
  const base::Vector<PassTiming>& results() const { return results_; }
  f32 total_ms() const { return total_ms_; }

 private:
  static constexpr u32 kMaxPasses = 48;
  static constexpr u32 kQueriesPerFrame = kMaxPasses * 2;

  struct FramePool {
    TimestampPoolHandle pool;
    base::Vector<std::string> names;  // one per pass, in record order
    u32 pass_count = 0;
    bool recorded = false;
  };

  Device* device_ = nullptr;
  f32 period_ns_ = 0.0f;
  base::Vector<FramePool> frames_;
  u32 current_ = 0;

  base::Vector<PassTiming> results_;
  f32 total_ms_ = 0.0f;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_GPU_PROFILER_H_
