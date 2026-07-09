#include "bethesda/tri.h"

#include <algorithm>
#include <cstring>

namespace rx::bethesda {
namespace {

constexpr char kMagic[8] = {'F', 'R', 'T', 'R', 'I', '0', '0', '3'};

struct Reader {
  ByteSpan data;
  size_t pos = 0;
  bool ok = true;

  template <typename T>
  T Read() {
    T value{};
    if (!ok || pos + sizeof(T) > data.size()) {
      ok = false;
      return value;
    }
    std::memcpy(&value, data.data() + pos, sizeof(T));
    pos += sizeof(T);
    return value;
  }

  const u8* Bytes(size_t count) {
    if (!ok || pos + count > data.size()) {
      ok = false;
      return nullptr;
    }
    const u8* p = data.data() + pos;
    pos += count;
    return p;
  }

  void Skip(size_t count) {
    if (!ok || pos + count > data.size()) ok = false;
    else pos += count;
  }
};

}  // namespace

const TriMorph* TriMorphSet::FindMorph(std::string_view name) const {
  for (const TriMorph& m : morphs)
    if (m.name == name) return &m;
  return nullptr;
}

std::optional<TriMorphSet> ParseTri(ByteSpan data) {
  Reader r{data};
  const u8* magic = r.Bytes(8);
  if (!magic || std::memcmp(magic, kMagic, 8) != 0) return std::nullopt;

  const u32 vertex_count = r.Read<u32>();
  const u32 face_count = r.Read<u32>();
  r.Skip(12);  // unknown_0c/10/14, always 0 in SE
  const u32 uv_count = r.Read<u32>();
  const u32 flags = r.Read<u32>();
  const u32 morph_count = r.Read<u32>();
  r.Read<u32>();  // modifier_count, always 0 in SE
  r.Read<u32>();  // modifier_vertex_count, always 0 in SE
  r.Skip(16);     // unknown_30..3c, always 0 in SE
  if (!r.ok) return std::nullopt;

  // Base geometry: vertices, then triangle vertex indices, then (when the UV
  // flag is set) uvs and uv indices. Only vertex_count is needed downstream,
  // the rest is skipped past to reach the morphs.
  r.Skip(static_cast<size_t>(vertex_count) * 3 * sizeof(f32));
  r.Skip(static_cast<size_t>(face_count) * 3 * sizeof(u32));
  if (flags & 1u) {
    r.Skip(static_cast<size_t>(uv_count) * 2 * sizeof(f32));
    r.Skip(static_cast<size_t>(face_count) * 3 * sizeof(u32));
  }
  if (!r.ok) return std::nullopt;

  TriMorphSet set;
  set.vertex_count = vertex_count;
  set.morphs.reserve(morph_count);
  for (u32 i = 0; i < morph_count; ++i) {
    const u32 name_len = r.Read<u32>();
    if (!r.ok || name_len == 0) return std::nullopt;
    const u8* name_bytes = r.Bytes(name_len);
    if (!name_bytes) return std::nullopt;
    const f32 scale = r.Read<f32>();

    TriMorph morph;
    // name_len counts the trailing null; drop it (and any padding nulls).
    size_t n = name_len;
    while (n > 0 && name_bytes[n - 1] == 0) --n;
    morph.name.assign(reinterpret_cast<const char*>(name_bytes), n);
    morph.scale = scale;

    morph.deltas.resize(vertex_count);
    for (u32 v = 0; v < vertex_count; ++v) {
      morph.deltas[v].x = r.Read<i16>();
      morph.deltas[v].y = r.Read<i16>();
      morph.deltas[v].z = r.Read<i16>();
    }
    if (!r.ok) return std::nullopt;
    set.morphs.push_back(std::move(morph));
  }
  return set;
}

void ApplyMorph(const TriMorph& morph, f32 weight, f32* positions, u32 vertex_count) {
  const u32 n = std::min(vertex_count, static_cast<u32>(morph.deltas.size()));
  const f32 s = weight * morph.scale;
  for (u32 i = 0; i < n; ++i) {
    positions[i * 3 + 0] += s * static_cast<f32>(morph.deltas[i].x);
    positions[i * 3 + 1] += s * static_cast<f32>(morph.deltas[i].y);
    positions[i * 3 + 2] += s * static_cast<f32>(morph.deltas[i].z);
  }
}

}  // namespace rx::bethesda
