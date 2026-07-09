#ifndef RECREATION_RUNTIME_FLY_CAMERA_H_
#define RECREATION_RUNTIME_FLY_CAMERA_H_

#include "core/input.h"
#include "core/input_actions.h"
#include "core/math.h"

namespace rx {

// Free flight debug camera. Hold the right mouse button (or push the right
// stick) to look around, move actions to fly, cam up/down for vertical, sprint
// to go fast. The scroll wheel scales the base speed.
class FlyCamera {
 public:
  void Update(const InputState& input, const ActionState& actions, bool allow_mouse,
              bool allow_keyboard, f32 dt);

  Vec3 position() const { return position_; }
  Vec3 forward() const;
  Vec3 target() const { return position_ + forward(); }
  bool looking() const { return looking_; }

  void set_position(const Vec3& position) { position_ = position; }
  void set_yaw_pitch(f32 yaw, f32 pitch) {
    yaw_ = yaw;
    pitch_ = pitch;
  }

  f32 speed = 3.0f;
  f32 sensitivity = 0.0025f;      // radians per mouse pixel
  f32 pad_sensitivity = 2.6f;     // radians per second at full stick deflection
  bool invert_y = false;

 private:
  Vec3 position_{2.4f, 1.8f, 2.4f};
  f32 yaw_ = -2.35f;  // pointed at the origin from the default position
  f32 pitch_ = -0.4f;
  bool looking_ = false;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_FLY_CAMERA_H_
