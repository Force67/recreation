#ifndef RECREATION_AUDIO_WAV_H_
#define RECREATION_AUDIO_WAV_H_

#include "audio/audio_clip.h"
#include "core/types.h"

namespace rec::audio {

// Decodes a RIFF/WAVE file into `out`. Handles the formats Bethesda's loose and
// archived .wav assets actually use: integer PCM (8/16/24/32-bit), IEEE float,
// WAVE_FORMAT_EXTENSIBLE, and the two ADPCM codecs (MS 0x0002, IMA/DVI 0x0011)
// that show up on compressed sound effects. Returns false on a truncated or
// unsupported file, leaving `out` cleared.
bool DecodeWav(ByteSpan bytes, AudioClip* out);

}  // namespace rec::audio

#endif  // RECREATION_AUDIO_WAV_H_
