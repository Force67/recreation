#include "components/bethesda/animation_data.h"

#include <base/memory/move.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include <cstdlib>

namespace rx::bethesda {
namespace {

// Line cursor over the text image. Lines are \r\n or \n terminated; a blank
// line separates blocks.
struct Lines {
  base::StringRef text;
  size_t pos = 0;

  bool Done() const { return pos >= text.size(); }
  base::StringRef Next() {
    size_t end = text.find('\n', pos);
    base::StringRef line = text.substr(pos, end == base::StringRef::npos ? end : end - pos);
    pos = end == base::StringRef::npos ? text.size() : end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
  }
};

f32 ToF32(base::StringRef s) { return std::strtof(base::String(s).c_str(), nullptr); }
i32 ToI32(base::StringRef s) {
  return static_cast<i32>(std::strtol(base::String(s).c_str(), nullptr, 10));
}

void ParseProject(base::StringRef text, AnimationData* out) {
  Lines lines{text};
  if (lines.Done()) return;
  lines.Next();  // leading count (always 1 in shipped data)
  i32 file_count = ToI32(lines.Next());
  for (i32 i = 0; i < file_count && !lines.Done(); ++i) lines.Next();
  if (lines.Done() || ToI32(lines.Next()) == 0) return;  // has-animation-data flag
  while (!lines.Done()) {
    base::StringRef name = lines.Next();
    if (name.empty()) continue;  // block separator
    ClipData clip;
    clip.name = base::String(name);
    clip.animation_index = ToI32(lines.Next());
    clip.playback_speed = ToF32(lines.Next());
    clip.crop_start = ToF32(lines.Next());
    clip.crop_end = ToF32(lines.Next());
    i32 events = ToI32(lines.Next());
    for (i32 e = 0; e < events && !lines.Done(); ++e) {
      base::StringRef line = lines.Next();
      size_t colon = line.rfind(':');
      if (colon == base::StringRef::npos) continue;
      clip.events.push_back({base::String(line.substr(0, colon)), ToF32(line.substr(colon + 1))});
    }
    out->clips.push_back(base::move(clip));
  }
}

MotionKey ParseKey(base::StringRef line, bool rotation) {
  MotionKey key;
  // strtof chains through the whitespace-separated floats.
  base::String owned(line);
  char* cursor = owned.data();
  key.time = std::strtof(cursor, &cursor);
  for (int i = 0; i < (rotation ? 4 : 3); ++i) {
    key.value[i] = std::strtof(cursor, &cursor);
  }
  return key;
}

void ParseMotion(base::StringRef text, AnimationData* out) {
  Lines lines{text};
  while (!lines.Done()) {
    base::StringRef id_line = lines.Next();
    if (id_line.empty()) continue;
    AnimMotion motion;
    i32 index = ToI32(id_line);
    motion.duration = ToF32(lines.Next());
    i32 t_count = ToI32(lines.Next());
    for (i32 i = 0; i < t_count && !lines.Done(); ++i) {
      motion.translation.push_back(ParseKey(lines.Next(), false));
    }
    i32 r_count = ToI32(lines.Next());
    for (i32 i = 0; i < r_count && !lines.Done(); ++i) {
      motion.rotation.push_back(ParseKey(lines.Next(), true));
    }
    out->motion.emplace(index, base::move(motion));
  }
}

}  // namespace

AnimationData ParseAnimationData(base::StringRef project_text, base::StringRef motion_text) {
  AnimationData data;
  ParseProject(project_text, &data);
  ParseMotion(motion_text, &data);
  return data;
}

Vec3 SampleMotionTranslation(const AnimMotion& motion, f32 time) {
  Vec3 prev{};  // implicit zero key at t=0
  f32 prev_t = 0;
  for (const MotionKey& key : motion.translation) {
    Vec3 value{key.value[0], key.value[1], key.value[2]};
    if (time <= key.time) {
      f32 span = key.time - prev_t;
      f32 a = span > 1e-6f ? (time - prev_t) / span : 1.0f;
      return Vec3{prev.x + (value.x - prev.x) * a, prev.y + (value.y - prev.y) * a,
                  prev.z + (value.z - prev.z) * a};
    }
    prev = value;
    prev_t = key.time;
  }
  return prev;  // past the last key: hold the final offset
}

Vec3 MotionTranslationDelta(const AnimMotion& motion, f32 t0, f32 t1) {
  if (t1 >= t0) {
    Vec3 a = SampleMotionTranslation(motion, t0);
    Vec3 b = SampleMotionTranslation(motion, t1);
    return Vec3{b.x - a.x, b.y - a.y, b.z - a.z};
  }
  // Looped through the end: tail of this cycle plus head of the next.
  Vec3 at0 = SampleMotionTranslation(motion, t0);
  Vec3 end = SampleMotionTranslation(motion, motion.duration);
  Vec3 at1 = SampleMotionTranslation(motion, t1);
  return Vec3{end.x - at0.x + at1.x, end.y - at0.y + at1.y, end.z - at0.z + at1.z};
}

}  // namespace rx::bethesda
