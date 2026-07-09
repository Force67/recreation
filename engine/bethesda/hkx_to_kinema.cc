#include "bethesda/hkx_to_kinema.h"

#include <algorithm>

namespace rx::bethesda {

std::vector<kinema::u8> TranscodeToKinema(const HkxAnimation& animation, const AnimMotion* motion,
                                          const std::vector<ClipEvent>* events) {
  const kinema::u32 tracks = animation.num_tracks;
  const kinema::u32 frames = std::max(animation.num_frames, 1u);
  const kinema::f32 rate =
      animation.frame_duration > 0 ? 1.0f / animation.frame_duration : 30.0f;

  kinema::ClipBuilder builder(tracks, frames, rate);
  builder.SetAdditive(animation.additive);

  std::vector<HkxTrackPose> pose;
  for (kinema::u32 f = 0; f < frames; ++f) {
    kinema::f32 time = std::min(static_cast<kinema::f32>(f) * animation.frame_duration,
                                animation.duration);
    SampleAnimation(animation, time, &pose);
    for (kinema::u32 t = 0; t < tracks && t < pose.size(); ++t) {
      const HkxTrackPose& s = pose[t];
      builder.SetSample(f, t,
                        kinema::Vec3{s.translation.x, s.translation.y, s.translation.z},
                        kinema::Quat{s.rotation[0], s.rotation[1], s.rotation[2], s.rotation[3]},
                        s.scale);
    }
  }
  if (motion) {
    for (const MotionKey& key : motion->translation) {
      builder.AddRootKey(key.time, kinema::Vec3{key.value[0], key.value[1], key.value[2]});
    }
  }
  if (events) {
    for (const ClipEvent& event : *events) builder.AddEvent(event.name, event.time);
  }
  return builder.Build();
}

}  // namespace rx::bethesda
