#include "dialogue/voice.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "asset/vfs.h"
#include "bethesda/load_order.h"
#include "bethesda/record.h"

namespace rx::dialogue {
namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

u32 FormIdAt(const bethesda::Subrecord* sub) {
  if (!sub || sub->data.size() < 4) return 0;
  u32 raw = 0;
  std::memcpy(&raw, sub->data.data(), 4);
  return raw;
}

}  // namespace

std::string VoiceFilePath(const std::string& plugin_file, const std::string& voice_type,
                          const std::string& quest_edid, const std::string& topic_edid,
                          u32 info_local_id, int response_index) {
  char tail[64];
  std::snprintf(tail, sizeof(tail), "_%08x_%d.fuz", info_local_id, response_index);
  return Lower("sound/voice/" + plugin_file + "/" + voice_type + "/" + quest_edid + "_" +
               topic_edid + tail);
}

std::vector<std::string> VoiceFileCandidates(const std::vector<std::string>& plugin_files,
                                             const std::string& voice_type,
                                             const std::string& quest_edid,
                                             const std::string& topic_edid, u32 info_local_id,
                                             int response_index) {
  std::vector<std::string> out;
  if (voice_type.empty()) return out;
  for (const std::string& plugin : plugin_files) {
    if (plugin.empty()) continue;
    out.push_back(
        VoiceFilePath(plugin, voice_type, quest_edid, topic_edid, info_local_id, response_index));
    // Scene dialogue is normally filed with an empty topic segment even when the
    // DIAL does carry an editor id, so probe that spelling too.
    if (!topic_edid.empty())
      out.push_back(VoiceFilePath(plugin, voice_type, quest_edid, {}, info_local_id,
                                  response_index));
    if (response_index != 1)
      out.push_back(VoiceFilePath(plugin, voice_type, quest_edid, topic_edid, info_local_id, 1));
  }
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string VoiceTypeEditorId(const bethesda::RecordStore& records, bethesda::GlobalFormId npc) {
  // An NPC that inherits its traits from a template carries no VTCK of its own,
  // so walk the TPLT chain (bounded, records can be authored into a cycle).
  bethesda::GlobalFormId current = npc;
  for (int depth = 0; depth < 8; ++depth) {
    bethesda::Record rec;
    if (!records.Parse(current, &rec)) return {};
    const bethesda::RecordStore::StoredRecord* stored = records.Find(current);
    const u16 plugin = stored ? stored->winning_plugin : current.plugin;
    if (u32 raw = FormIdAt(rec.Find(FourCc('V', 'T', 'C', 'K')))) {
      bethesda::Record voice;
      if (records.Parse(records.ResolveFrom(bethesda::RawFormId{raw}, plugin), &voice))
        return Lower(voice.GetString(FourCc('E', 'D', 'I', 'D')));
      return {};
    }
    const u32 tplt = FormIdAt(rec.Find(FourCc('T', 'P', 'L', 'T')));
    if (!tplt) return {};
    current = records.ResolveFrom(bethesda::RawFormId{tplt}, plugin);
  }
  return {};
}

namespace {

u32 ReadU32(ByteSpan bytes, size_t offset) {
  if (offset + 4 > bytes.size()) return 0;
  u32 value = 0;
  std::memcpy(&value, bytes.data() + offset, 4);
  return value;
}

u16 ReadU16(ByteSpan bytes, size_t offset) {
  if (offset + 2 > bytes.size()) return 0;
  u16 value = 0;
  std::memcpy(&value, bytes.data() + offset, 2);
  return value;
}

bool TagAt(ByteSpan bytes, size_t offset, const char* tag) {
  if (offset + 4 > bytes.size()) return false;
  return std::memcmp(bytes.data() + offset, tag, 4) == 0;
}

}  // namespace

bool VoiceIndex::ParsePath(std::string_view path, u32* info_local_id, std::string* voice_type) {
  // sound/voice/<plugin>/<voice type>/<quest>_<topic>_<info hex8>_<n>.fuz
  if (path.size() < 8 || path.rfind(".fuz") != path.size() - 4) return false;
  const size_t slash = path.rfind('/');
  if (slash == std::string_view::npos || slash == 0) return false;
  const size_t voice_start = path.rfind('/', slash - 1);
  if (voice_start == std::string_view::npos) return false;
  const std::string_view name = path.substr(slash + 1, path.size() - slash - 5);  // drop ".fuz"
  // The id is the second-to-last underscore-separated field.
  const size_t last = name.rfind('_');
  if (last == std::string_view::npos || last == 0) return false;
  const size_t prev = name.rfind('_', last - 1);
  if (prev == std::string_view::npos) return false;
  const std::string_view id = name.substr(prev + 1, last - prev - 1);
  if (id.size() != 8) return false;
  u32 value = 0;
  for (char c : id) {
    const int digit = (c >= '0' && c <= '9')   ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                              : -1;
    if (digit < 0) return false;
    value = value * 16 + static_cast<u32>(digit);
  }
  *info_local_id = value;
  voice_type->assign(path.substr(voice_start + 1, slash - voice_start - 1));
  return true;
}

void VoiceIndex::Build(const asset::Vfs& vfs) {
  if (built_) return;
  built_ = true;
  vfs.Enumerate([&](std::string_view path) {
    if (path.rfind("sound/voice/", 0) != 0) return;
    u32 info = 0;
    std::string voice_type;
    if (!ParsePath(path, &info, &voice_type)) return;
    by_info_[info].push_back({std::move(voice_type), std::string(path)});
  });
}

std::string VoiceIndex::Find(u32 info_local_id, const std::string& voice_type) const {
  auto it = by_info_.find(info_local_id);
  if (it == by_info_.end()) return {};
  const std::string want = Lower(voice_type);
  for (const Clip& clip : it->second)
    if (clip.voice_type == want) return clip.path;
  // A line recorded for one voice type only (a unique actor) still belongs to this
  // INFO, so it is the right recording even when the speaker's type does not match.
  return it->second.front().path;
}

f32 ClipSeconds(ByteSpan bytes) {
  size_t at = 0;
  // FUZE: magic, version, lipsync block size, then the wrapped RIFF.
  if (TagAt(bytes, 0, "FUZE")) at = 12 + ReadU32(bytes, 8);
  if (!TagAt(bytes, at, "RIFF")) return 0.0f;
  const bool xwma = TagAt(bytes, at + 8, "XWMA");
  if (!xwma && !TagAt(bytes, at + 8, "WAVE")) return 0.0f;

  u32 sample_rate = 0, byte_rate = 0, data_size = 0, decoded_bytes = 0;
  u16 channels = 0, bits = 0;
  size_t chunk = at + 12;
  while (chunk + 8 <= bytes.size()) {
    const u32 size = ReadU32(bytes, chunk + 4);
    const size_t body = chunk + 8;
    if (TagAt(bytes, chunk, "fmt ")) {
      channels = ReadU16(bytes, body + 2);
      sample_rate = ReadU32(bytes, body + 4);
      byte_rate = ReadU32(bytes, body + 8);
      bits = ReadU16(bytes, body + 14);
    } else if (TagAt(bytes, chunk, "dpds")) {
      // Cumulative decoded bytes per packet; the last entry is the whole clip.
      if (size >= 4) decoded_bytes = ReadU32(bytes, body + size - 4);
    } else if (TagAt(bytes, chunk, "data")) {
      data_size = size;
    }
    chunk = body + size + (size & 1);  // chunks are word aligned
  }
  if (sample_rate == 0) return 0.0f;
  if (decoded_bytes > 0 && channels > 0 && bits >= 8) {
    const u32 frame_bytes = channels * (bits / 8u);
    if (frame_bytes > 0)
      return static_cast<f32>(decoded_bytes / frame_bytes) / static_cast<f32>(sample_rate);
  }
  if (data_size > 0 && byte_rate > 0)
    return static_cast<f32>(data_size) / static_cast<f32>(byte_rate);
  return 0.0f;
}

f32 EstimateLineSeconds(const std::string& text) {
  // ~14 characters a second reads like spoken delivery; the floor keeps a short
  // interjection on screen and the ceiling stops a paragraph from parking a phase.
  const f32 read = 0.9f + static_cast<f32>(text.size()) / 14.0f;
  return std::clamp(read, 1.6f, 9.0f);
}

}  // namespace rx::dialogue
