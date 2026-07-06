#ifndef RECREATION_BETHESDA_HKX_TO_KINEMA_H_
#define RECREATION_BETHESDA_HKX_TO_KINEMA_H_

// Transcodes a decoded Havok spline animation into a kinema clip blob:
// the splines are sampled once at the clip's native uniform frame rate and
// quantized into kinema's flat SoA format, so runtime sampling never touches
// de Boor again. Root motion and trigger events (Bethesda sidecar data)
// ride along in the blob.

#include <vector>

#include <kinema/kinema.h>

#include "bethesda/animation_data.h"
#include "bethesda/hkx_anim.h"

namespace rec::bethesda {

// `motion` / `events` are optional (from the animationdata sidecars).
std::vector<kinema::u8> TranscodeToKinema(const HkxAnimation& animation, const AnimMotion* motion,
                                          const std::vector<ClipEvent>* events);

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_HKX_TO_KINEMA_H_
