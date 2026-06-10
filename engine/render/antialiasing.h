#ifndef RECREATION_RENDER_ANTIALIASING_H_
#define RECREATION_RENDER_ANTIALIASING_H_

#include "core/types.h"

namespace rec::render {

enum class AntiAliasingMode : u8 {
  kNone,
  kTaa,
  // Upscalers do their own temporal accumulation, TAA must be off when one
  // is active. The renderer enforces this.
  kUpscaler,
};

struct JitterSequence {
  // Halton (2,3) offsets in pixel units, centered around zero.
  static void Sample(u32 frame_index, u32 sample_count, f32* out_x, f32* out_y);
};

class TaaPass {
 public:
  struct Settings {
    f32 history_blend = 0.9f;
    bool variance_clipping = true;
    u32 jitter_sample_count = 8;
  };

  void Configure(const Settings& settings) { settings_ = settings; }
  void Reset();

  // Records the resolve into the render graph. Needs color, depth, motion
  // vectors and the history target from the previous frame.
  void AddToGraph(class RenderGraph& graph, u32 frame_index);

  const Settings& settings() const { return settings_; }

 private:
  Settings settings_;
  bool history_valid_ = false;
};

}  // namespace rec::render

#endif  // RECREATION_RENDER_ANTIALIASING_H_
