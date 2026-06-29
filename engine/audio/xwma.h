#ifndef RECREATION_AUDIO_XWMA_H_
#define RECREATION_AUDIO_XWMA_H_

#include <memory>

#include "audio/audio_clip.h"
#include "core/types.h"

namespace rec::audio {

// The compressed audio containers the three games ship: Skyrim/Fallout 4 store
// music and ambience as xWMA (.xwm), voice lines as Bethesda's FUZ wrapper (a LIP
// lipsync block followed by an embedded xWMA), and Starfield uses Wwise media
// (.wem).
enum class CompressedKind { kXwma, kFuz, kWem };

// Opens a streaming decoder for a compressed container. PCM-tagged Wwise media is
// decoded natively; the WMA and Wwise codecs are routed to the optional FFmpeg
// backend (see ffmpeg_codec.h), which is null when the engine was built without
// it. Returns null when nothing can decode the data.
std::unique_ptr<Decoder> OpenCompressed(ByteSpan bytes, CompressedKind kind);

}  // namespace rec::audio

#endif  // RECREATION_AUDIO_XWMA_H_
