#include "audio/ffmpeg_codec.h"

#include "core/log.h"

// Compiled when the FFmpeg backend is off (the default, and the whole CI matrix):
// compressed audio that has no native decoder is skipped with a single, clear
// note rather than failing the build or crashing at runtime.
namespace rec::audio {

std::unique_ptr<Decoder> OpenFfmpegDecoder(ByteSpan) {
  static bool warned = false;
  if (!warned) {
    warned = true;
    REC_WARN(
        "audio: compressed formats (xWMA/FUZ/Wwise) need the FFmpeg backend; "
        "reconfigure with -DRECREATION_AUDIO_FFMPEG=ON to hear them");
  }
  return nullptr;
}

bool FfmpegAvailable() { return false; }

}  // namespace rec::audio
