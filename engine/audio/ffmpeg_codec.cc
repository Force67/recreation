#include "audio/ffmpeg_codec.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

// The real FFmpeg backend: demux + decode a compressed container fully into an
// interleaved-float AudioClip, then play it back through the shared clip decoder.
// Decoding up front keeps the libav lifetime tidy (everything is freed before the
// mixer ever touches the data) and is plenty for the ambience loops, music tracks
// and voice lines these formats carry.
namespace rec::audio {
namespace {

// Feeds the in-memory container to libavformat through a custom AVIO buffer, so no
// temp file is needed.
struct MemorySource {
  const u8* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
};

int ReadPacket(void* opaque, u8* buf, int want) {
  auto* src = static_cast<MemorySource*>(opaque);
  const size_t remaining = src->size - src->pos;
  if (remaining == 0) return AVERROR_EOF;
  const int n = static_cast<int>(want < static_cast<int>(remaining) ? want : remaining);
  std::memcpy(buf, src->data + src->pos, static_cast<size_t>(n));
  src->pos += static_cast<size_t>(n);
  return n;
}

int64_t SeekPacket(void* opaque, int64_t offset, int whence) {
  auto* src = static_cast<MemorySource*>(opaque);
  if (whence == AVSEEK_SIZE) return static_cast<int64_t>(src->size);
  int64_t target = offset;
  if (whence == SEEK_CUR) target = static_cast<int64_t>(src->pos) + offset;
  else if (whence == SEEK_END) target = static_cast<int64_t>(src->size) + offset;
  if (target < 0 || target > static_cast<int64_t>(src->size)) return -1;
  src->pos = static_cast<size_t>(target);
  return target;
}

// RAII for the libav handles, so every early return cleans up.
struct Decode {
  AVIOContext* avio = nullptr;
  AVFormatContext* fmt = nullptr;
  AVCodecContext* codec = nullptr;
  SwrContext* swr = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  u8* avio_buffer = nullptr;

  ~Decode() {
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    if (swr) swr_free(&swr);
    if (codec) avcodec_free_context(&codec);
    if (fmt) avformat_close_input(&fmt);
    if (avio) {
      // avio owns avio_buffer after av_malloc; free through the context.
      av_freep(&avio->buffer);
      avio_context_free(&avio);
    }
  }
};

}  // namespace

std::unique_ptr<Decoder> OpenFfmpegDecoder(ByteSpan bytes) {
  if (bytes.empty()) return nullptr;
  MemorySource source{bytes.data(), bytes.size(), 0};
  Decode d;

  constexpr int kBufferSize = 32 * 1024;
  d.avio_buffer = static_cast<u8*>(av_malloc(kBufferSize));
  if (!d.avio_buffer) return nullptr;
  d.avio = avio_alloc_context(d.avio_buffer, kBufferSize, 0, &source, ReadPacket, nullptr,
                              SeekPacket);
  if (!d.avio) {
    av_freep(&d.avio_buffer);
    return nullptr;
  }

  d.fmt = avformat_alloc_context();
  if (!d.fmt) return nullptr;
  d.fmt->pb = d.avio;
  d.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  if (avformat_open_input(&d.fmt, nullptr, nullptr, nullptr) < 0) {
    REC_WARN("audio: FFmpeg could not open the stream");
    return nullptr;
  }
  if (avformat_find_stream_info(d.fmt, nullptr) < 0) return nullptr;

  const AVCodec* decoder = nullptr;
  const int stream_index = av_find_best_stream(d.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
  if (stream_index < 0 || !decoder) return nullptr;
  AVStream* stream = d.fmt->streams[stream_index];

  d.codec = avcodec_alloc_context3(decoder);
  if (!d.codec) return nullptr;
  if (avcodec_parameters_to_context(d.codec, stream->codecpar) < 0) return nullptr;
  if (avcodec_open2(d.codec, decoder, nullptr) < 0) {
    REC_WARN("audio: FFmpeg has no decoder for this codec");
    return nullptr;
  }

  // Output interleaved float, native rate, stereo at most (the mixer resamples and
  // pans; sources with more channels are downmixed to stereo here).
  const int out_channels = d.codec->ch_layout.nb_channels >= 2 ? 2 : 1;
  AVChannelLayout out_layout;
  av_channel_layout_default(&out_layout, out_channels);
  const int out_rate = d.codec->sample_rate;

  if (swr_alloc_set_opts2(&d.swr, &out_layout, AV_SAMPLE_FMT_FLT, out_rate, &d.codec->ch_layout,
                          d.codec->sample_fmt, d.codec->sample_rate, 0, nullptr) < 0 ||
      swr_init(d.swr) < 0) {
    av_channel_layout_uninit(&out_layout);
    return nullptr;
  }

  d.packet = av_packet_alloc();
  d.frame = av_frame_alloc();
  if (!d.packet || !d.frame) {
    av_channel_layout_uninit(&out_layout);
    return nullptr;
  }

  AudioClip clip;
  clip.channels = static_cast<u32>(out_channels);
  clip.sample_rate = static_cast<u32>(out_rate);
  std::vector<float> convert;

  auto drain = [&](bool flush) {
    while (true) {
      const int rc = avcodec_receive_frame(d.codec, d.frame);
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
      if (rc < 0) return false;
      const int max_out = swr_get_out_samples(d.swr, d.frame->nb_samples);
      convert.resize(static_cast<size_t>(max_out) * out_channels);
      u8* out_ptr = reinterpret_cast<u8*>(convert.data());
      const int got = swr_convert(d.swr, &out_ptr, max_out,
                                  const_cast<const u8**>(d.frame->extended_data),
                                  d.frame->nb_samples);
      if (got < 0) return false;
      clip.samples.insert(clip.samples.end(), convert.begin(),
                          convert.begin() + static_cast<size_t>(got) * out_channels);
    }
    (void)flush;
    return true;
  };

  bool ok = true;
  while (av_read_frame(d.fmt, d.packet) >= 0) {
    if (d.packet->stream_index == stream_index) {
      if (avcodec_send_packet(d.codec, d.packet) >= 0) ok = drain(false);
    }
    av_packet_unref(d.packet);
    if (!ok) break;
  }
  if (ok) {
    avcodec_send_packet(d.codec, nullptr);  // flush
    drain(true);
  }
  av_channel_layout_uninit(&out_layout);

  if (!clip.valid()) {
    REC_WARN("audio: FFmpeg produced no samples");
    return nullptr;
  }
  REC_INFO("audio: FFmpeg decoded {} frames, {} ch, {} Hz", clip.frames(), clip.channels,
           clip.sample_rate);
  return MakeClipDecoder(std::move(clip));
}

bool FfmpegAvailable() { return true; }

}  // namespace rec::audio
