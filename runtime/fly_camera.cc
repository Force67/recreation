#include "fly_camera.h"

#include <algorithm>
#include <cmath>

namespace rx {

Vec3 FlyCamera::forward() const {
  return {std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_), -std::cos(pitch_) * std::cos(yaw_)};
}

void FlyCamera::Update(const InputState& input, const ActionState& actions, bool allow_mouse,
                       bool allow_keyboard, f32 dt) {
  looking_ = allow_mouse && input.button(MouseButton::kRight);

  if (looking_) {
    yaw_ += input.mouse_dx * sensitivity;
    pitch_ -= input.mouse_dy * sensitivity;
    if (input.wheel != 0) {
      speed *= std::pow(1.2f, input.wheel);
      speed = std::clamp(speed, 0.1f, 200.0f);
    }
  }
  // Right-stick look (rate based), no button needed.
  if (allow_keyboard) {
    yaw_ += actions.axis(Axis::kLookX) * pad_sensitivity * dt;
    pitch_ -= actions.axis(Axis::kLookY) * pad_sensitivity * dt * (invert_y ? -1.0f : 1.0f);
  }
  pitch_ = std::clamp(pitch_, -1.55f, 1.55f);

  if (!allow_keyboard) return;

  Vec3 fwd = forward();
  Vec3 right = Normalize(Cross(fwd, {0, 1, 0}));
  Vec3 move{};
  // Combined keyboard + left-stick planar movement.
  move += fwd * -actions.axis(Axis::kMoveY);  // stick-up / W = forward
  move += right * actions.axis(Axis::kMoveX);
  if (actions.down(Action::kCamUp) || actions.down(Action::kJump)) move += Vec3{0, 1, 0};
  if (actions.down(Action::kCamDown) || actions.down(Action::kSneak)) move += Vec3{0, -1, 0};

  f32 length = std::sqrt(Dot(move, move));
  if (length > 0) {
    f32 boost = actions.down(Action::kSprint) ? 4.0f : 1.0f;
    position_ += move * (speed * boost * dt / length);
  }
}

}  // namespace rx
