#ifndef RECREATION_BETHESDA_HKX_TO_KINEMA_H_
#define RECREATION_BETHESDA_HKX_TO_KINEMA_H_

// Transcodes a decoded Havok spline animation into a kinema clip blob:
// the splines are sampled once at the clip's native uniform frame rate and
// quantized into kinema's flat SoA format, so runtime sampling never touches
// de Boor again. Root motion and trigger events (Bethesda sidecar data)
// ride along in the blob.

#include <base/containers/vector.h>
#include <kinema/kinema.h>

#include "asset/skeleton.h"
#include "components/bethesda/animation_data.h"
#include "components/bethesda/hkx_anim.h"

namespace rx::bethesda {

// `motion` / `events` are optional (from the animationdata sidecars).
base::Vector<kinema::u8> TranscodeToKinema(const HkxAnimation& animation, const AnimMotion* motion,
                                           const base::Vector<ClipEvent>* events);

// Skeleton-space transcode: instead of one track per animation channel (havok
// order), the blob carries one track per skeleton bone. Each sampled frame is
// scattered through `track_to_skeleton` (track -> skeleton bone, -1 = drop) onto
// a frame pre-filled with the skeleton's bind pose, so untouched bones hold rest
// and the clip's track count equals `skeleton.bones.size()`. This lets a
// kinema::StateMachine / additive layer built over the skeleton drive the actor
// pose directly, with no per-frame track remap. Root motion + events ride along
// exactly as TranscodeToKinema.
base::Vector<kinema::u8> TranscodeToKinemaSkeleton(const HkxAnimation& animation,
                                                   const base::Vector<i32>& track_to_skeleton,
                                                   const asset::Skeleton& skeleton,
                                                   const AnimMotion* motion,
                                                   const base::Vector<ClipEvent>* events);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_HKX_TO_KINEMA_H_
