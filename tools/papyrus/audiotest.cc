// audiotest: the audio subsystem's portable core -- the native WAV decoder, the
// software mixer's resampling and voice lifecycle, and the 3D pan/attenuation
// math. No device and no game data, so it runs in the default ctest gate and
// guards the parts that never touch SDL or a sound card.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "audio/audio_clip.h"
#include "audio/mixer.h"
#include "audio/spatial.h"
#include "audio/wav.h"

using namespace rec;
using namespace rec::audio;

namespace {

void PutU16(std::vector<std::uint8_t>& b, std::uint16_t v) {
  b.push_back(v & 0xFF);
  b.push_back(v >> 8);
}
void PutU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF);
}
void PutTag(std::vector<std::uint8_t>& b, const char* t) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(t[i]));
}

// A mono 16-bit PCM WAV holding `frames` samples of a unit-amplitude sine.
std::vector<std::uint8_t> MakeSineWav(std::uint32_t rate, std::uint32_t frames, double hz) {
  std::vector<std::uint8_t> data;
  for (std::uint32_t i = 0; i < frames; ++i) {
    double t = static_cast<double>(i) / rate;
    auto s = static_cast<std::int16_t>(std::sin(2.0 * 3.14159265 * hz * t) * 30000.0);
    PutU16(data, static_cast<std::uint16_t>(s));
  }
  std::vector<std::uint8_t> w;
  PutTag(w, "RIFF");
  PutU32(w, 36 + static_cast<std::uint32_t>(data.size()));
  PutTag(w, "WAVE");
  PutTag(w, "fmt ");
  PutU32(w, 16);
  PutU16(w, 1);             // PCM
  PutU16(w, 1);             // mono
  PutU32(w, rate);
  PutU32(w, rate * 2);      // byte rate
  PutU16(w, 2);             // block align
  PutU16(w, 16);            // bits
  PutTag(w, "data");
  PutU32(w, static_cast<std::uint32_t>(data.size()));
  w.insert(w.end(), data.begin(), data.end());
  return w;
}

double Rms(const std::vector<float>& buf) {
  if (buf.empty()) return 0.0;
  double acc = 0;
  for (float s : buf) acc += static_cast<double>(s) * s;
  return std::sqrt(acc / buf.size());
}

}  // namespace

int main() {
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  // --- WAV decode --------------------------------------------------------------
  const std::uint32_t kRate = 22050, kFrames = 11025;  // half a second
  auto wav = MakeSineWav(kRate, kFrames, 440.0);
  AudioClip clip;
  bool decoded = DecodeWav(ByteSpan{wav.data(), wav.size()}, &clip);
  check("PCM16 WAV decodes", decoded);
  check("decoded channel count", clip.channels == 1);
  check("decoded sample rate", clip.sample_rate == kRate);
  check("decoded frame count", clip.frames() == kFrames);
  check("decoded samples in [-1,1]", !clip.samples.empty() && std::fabs(clip.samples[100]) <= 1.0f);

  // Garbage and truncation fail cleanly rather than crashing or succeeding.
  AudioClip junk;
  std::vector<std::uint8_t> garbage(64, 0xAB);
  check("garbage WAV rejected", !DecodeWav(ByteSpan{garbage.data(), garbage.size()}, &junk));
  check("truncated WAV rejected",
        !DecodeWav(ByteSpan{wav.data(), 20}, &junk));

  // --- mixer: resample + lifecycle --------------------------------------------
  Mixer mixer;
  mixer.Configure(48000);
  PlayParams params;  // a 2D one-shot
  std::uint32_t voice = mixer.Play(MakeClipDecoder(clip), params);
  check("voice id allocated", voice != 0);

  std::vector<float> out(2048 * 2);
  mixer.MixInto(out.data(), 2048);  // first block applies the play command
  check("first block produces sound", Rms(out) > 0.01);

  // Drain to the end: a 0.5 s source upsampled to 48 kHz is ~24000 frames, so a
  // few thousand more blocks must eventually fall silent (the voice ends).
  bool went_silent = false;
  for (int i = 0; i < 64 && !went_silent; ++i) {
    mixer.MixInto(out.data(), 2048);
    if (Rms(out) < 1e-6) went_silent = true;
  }
  check("one-shot voice ends and goes silent", went_silent);

  // A looping voice keeps producing sound well past the source length.
  Mixer loop_mixer;
  loop_mixer.Configure(48000);
  PlayParams loop_params;
  loop_params.loop = true;
  loop_mixer.Play(MakeClipDecoder(clip), loop_params);
  bool always_sound = true;
  for (int i = 0; i < 64; ++i) {
    loop_mixer.MixInto(out.data(), 2048);
    if (Rms(out) < 1e-6) always_sound = false;
  }
  check("looping voice never goes silent", always_sound);

  // --- spatialization ----------------------------------------------------------
  Attenuation atten;  // ref 4, max 60
  check("gain is full within ref distance", DistanceGain(2.0f, atten) == 1.0f);
  check("gain is zero past max distance", DistanceGain(80.0f, atten) == 0.0f);
  check("gain falls off with distance",
        DistanceGain(10.0f, atten) > DistanceGain(30.0f, atten) &&
            DistanceGain(30.0f, atten) > 0.0f);

  Listener listener;  // at origin, facing -Z, Y up -> +X is to the right
  StereoGains right = PanForSource(listener, Vec3{8, 0, 0}, atten);
  StereoGains left = PanForSource(listener, Vec3{-8, 0, 0}, atten);
  check("source on the right is louder in the right ear", right.right > right.left);
  check("source on the left is louder in the left ear", left.left > left.right);
  StereoGains centre = PanForSource(listener, Vec3{0, 0, -8}, atten);
  check("centred source is balanced", std::fabs(centre.left - centre.right) < 0.05f);

  std::printf("audiotest: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
