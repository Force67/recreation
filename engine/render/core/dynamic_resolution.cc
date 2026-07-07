#include "render/core/dynamic_resolution.h"

#include <algorithm>
#include <cmath>

namespace rec::render {

bool DynamicResolution::Update(f32 gpu_ms) {
  if (gpu_ms <= 0.0f || settings_.target_ms <= 0.0f) return false;
  ema_ms_ = ema_ms_ <= 0.0f ? gpu_ms : ema_ms_ + 0.1f * (gpu_ms - ema_ms_);
  if (cooldown_ > 0) {
    --cooldown_;
    return false;
  }

  const f32 target = settings_.target_ms;
  const f32 min_scale = std::clamp(settings_.min_scale, 0.25f, 1.0f);

  if (ema_ms_ > target * kOvershoot) {
    under_ = 0;
    if (scale_ <= min_scale + 1e-4f) {
      over_ = 0;  // pinned at the floor; nothing left to trade
      return false;
    }
    if (++over_ < kDownFrames) return false;
    // Cost is ~quadratic in the per-axis scale: jump to the step predicted to
    // land just under the target instead of walking down one step per cycle.
    f32 fit = scale_ * std::sqrt(target * 0.95f / ema_ms_);
    f32 next = std::floor(fit / kStep) * kStep;
    return Apply(std::clamp(next, min_scale, scale_ - kStep));
  }

  over_ = 0;
  f32 raised = std::min(1.0f, scale_ + kStep);
  if (raised > scale_ + 1e-4f) {
    // The predicted cost at the next step up must still fit with margin, or
    // the controller would oscillate across the target.
    f32 predicted = ema_ms_ * (raised / scale_) * (raised / scale_);
    if (predicted < target * kHeadroom) {
      if (++under_ >= kUpFrames) return Apply(raised);
      return false;
    }
  }
  under_ = 0;
  return false;
}

bool DynamicResolution::Apply(f32 next) {
  next = std::round(next / kStep) * kStep;  // stay on the grid, no float drift
  if (std::abs(next - scale_) < 1e-4f) {
    over_ = under_ = 0;
    return false;
  }
  scale_ = next;
  ema_ms_ = 0.0f;  // samples from the old resolution no longer describe the cost
  over_ = under_ = 0;
  cooldown_ = kCooldownFrames;
  return true;
}

void DynamicResolution::Reset() {
  scale_ = 1.0f;
  ema_ms_ = 0.0f;
  over_ = under_ = cooldown_ = 0;
}

}  // namespace rec::render
