#ifndef RECREATION_AUDIO_FFMPEG_CODEC_H_
#define RECREATION_AUDIO_FFMPEG_CODEC_H_

#include <memory>

#include "audio/audio_clip.h"
#include "core/types.h"

namespace rec::audio {

// Decodes a complete compressed container (an xWMA RIFF, or anything libavformat
// can demux) into a decoder, using FFmpeg's libav* libraries. This is the backend
// for the codecs with no lightweight decoder of their own -- WMA behind xWMA/FUZ,
// and the compressed Wwise codecs.
//
// Two implementations are linked exclusively: the real one (ffmpeg_codec.cc, when
// RECREATION_AUDIO_FFMPEG is on and libav* is found) and a stub (ffmpeg_stub.cc)
// that returns null and explains how to enable the backend. Keeping FFmpeg behind
// a build option and out of the default build means the CI matrix stays free of a
// heavyweight system dependency; enable it (and ship its shared libraries beside
// the executable) to hear the games' compressed music, ambience and voice.
std::unique_ptr<Decoder> OpenFfmpegDecoder(ByteSpan bytes);

// Whether the FFmpeg backend is compiled in. False for the stub.
bool FfmpegAvailable();

}  // namespace rec::audio

#endif  // RECREATION_AUDIO_FFMPEG_CODEC_H_
