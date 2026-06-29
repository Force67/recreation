#include "audio/audio_system.h"

#include <algorithm>

#include <base/option.h>

#include "asset/vfs.h"
#include "core/log.h"

namespace rec::audio {
namespace {

// Suppression + level controls. Namespace scope so they register before
// InitOptionsFromEnv() runs (see base::Option). audio.mute fully suppresses the
// subsystem; audio.volume sets the master level.
base::Option<bool> Mute{"audio.mute", false, "REC_AUDIO_MUTE",
                        "open no audio device and silence all playback"};
base::Option<float> Volume{"audio.volume", 1.0f, "REC_AUDIO_VOLUME",
                           "master output volume, 0..1"};

// Pulls the extension (including the dot) off a path for decoder dispatch.
std::string_view ExtensionOf(std::string_view path) {
  const size_t dot = path.find_last_of('.');
  const size_t slash = path.find_last_of("/\\");
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
    return {};
  return path.substr(dot);
}

}  // namespace

bool AudioSystem::Initialize(asset::Vfs* vfs) {
  vfs_ = vfs;
  muted_ = Mute.get();
  master_ = std::clamp(Volume.get(), 0.0f, 1.0f);

  if (muted_) {
    REC_INFO("audio: suppressed (REC_AUDIO_MUTE), running silent");
    return false;
  }
  const bool ok = device_.Open(&mixer_);
  mixer_.SetMasterGain(master_);
  return ok;
}

void AudioSystem::Shutdown() {
  mixer_.StopAll();
  device_.Close();
  clip_cache_.clear();
}

void AudioSystem::SetListener(const Vec3& position, const Vec3& forward, const Vec3& up) {
  // No device means nothing drains the mixer command queue, so skip enqueuing
  // (a headless server would otherwise grow the queue every frame).
  if (!active()) return;
  Listener listener;
  listener.position = position;
  listener.forward = forward;
  listener.up = up;
  mixer_.SetListener(listener);
}

bool AudioSystem::ReadAsset(std::string_view path, std::vector<u8>* out) {
  out->clear();
  if (!vfs_) return false;
  std::optional<base::Vector<u8>> bytes = vfs_->Read(path);
  if (!bytes || bytes->size() == 0) return false;
  out->assign(bytes->data(), bytes->data() + bytes->size());
  return true;
}

const AudioClip* AudioSystem::GetClip(std::string_view path) {
  const std::string key(path);
  auto it = clip_cache_.find(key);
  if (it != clip_cache_.end()) return it->second.valid() ? &it->second : nullptr;

  std::vector<u8> bytes;
  AudioClip clip;
  if (ReadAsset(path, &bytes))
    clip = DecodeClip(ByteSpan{bytes.data(), bytes.size()}, ExtensionOf(path));
  if (!clip.valid()) REC_WARN("audio: could not decode '{}'", key);
  auto [inserted, _] = clip_cache_.emplace(key, std::move(clip));
  return inserted->second.valid() ? &inserted->second : nullptr;
}

std::unique_ptr<Decoder> AudioSystem::OpenStream(std::string_view path) {
  std::vector<u8> bytes;
  if (!ReadAsset(path, &bytes)) return nullptr;
  return OpenDecoder(ByteSpan{bytes.data(), bytes.size()}, ExtensionOf(path));
}

u32 AudioSystem::PlayUi(std::string_view path, f32 gain) {
  if (!active()) return 0;
  const AudioClip* clip = GetClip(path);
  if (!clip) return 0;
  PlayParams params;
  params.gain = gain;
  params.positional = false;
  return mixer_.Play(MakeClipDecoder(*clip), params);
}

u32 AudioSystem::PlayAt(std::string_view path, const Vec3& position, PlayParams params) {
  if (!active()) return 0;
  const AudioClip* clip = GetClip(path);
  if (!clip) return 0;
  params.positional = true;
  params.position = position;
  return mixer_.Play(MakeClipDecoder(*clip), params);
}

u32 AudioSystem::PlayLoop(std::string_view path, PlayParams params) {
  if (!active()) return 0;
  std::unique_ptr<Decoder> decoder = OpenStream(path);
  if (!decoder) return 0;
  params.loop = true;
  return mixer_.Play(std::move(decoder), params);
}

void AudioSystem::Stop(u32 voice, f32 fade) {
  if (voice) mixer_.Stop(voice, fade);
}

void AudioSystem::StopAll() { mixer_.StopAll(); }

void AudioSystem::SetVoicePosition(u32 voice, const Vec3& position) {
  if (voice) mixer_.SetVoicePosition(voice, position);
}

void AudioSystem::SetVoiceGain(u32 voice, f32 gain) {
  if (voice) mixer_.SetVoiceGain(voice, gain);
}

void AudioSystem::SetMasterVolume(f32 volume) {
  master_ = std::clamp(volume, 0.0f, 1.0f);
  mixer_.SetMasterGain(master_);
}

}  // namespace rec::audio
