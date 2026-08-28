#ifndef RECREATION_SCRIPT_VEHICLE_DRIVE_SINK_H_
#define RECREATION_SCRIPT_VEHICLE_DRIVE_SINK_H_

#include "core/types.h"

namespace rx::script {

// Optional script-facing hook into the host's cart ride, so the Vehicle natives
// can steer the ridden cart (the "super cart" racing kit). Set by the runtime
// when a carriage system is up; null keeps the natives neutral no-ops. Called
// on the guest thread, so the implementation must be thread-safe (the runtime
// routes it to the main thread's carriage system).
class VehicleDriveSink {
 public:
  virtual ~VehicleDriveSink() = default;

  // Commands the ridden cart to steer [-1,1] and throttle [0,1] for its next
  // drive step, overriding the journey path-follow while that ride lasts.
  virtual void DriveCart(f32 steer, f32 throttle) = 0;

  // Snaps the ridden ride back to a game-space position (x, up y, z) so a mod
  // can implement respawn-to-checkpoint. Neutral when nothing is ridden.
  virtual void MoveRidden(f32 x, f32 y, f32 z) {}
};

}  // namespace rx::script

#endif  // RECREATION_SCRIPT_VEHICLE_DRIVE_SINK_H_