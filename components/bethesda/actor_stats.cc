#include "components/bethesda/actor_stats.h"

#include <cstring>

#include "components/bethesda/plugin.h"

namespace rx::bethesda {
namespace {

constexpr u32 kAcbs = FourCc('A', 'C', 'B', 'S');
constexpr u32 kAidt = FourCc('A', 'I', 'D', 'T');

// ACBS field offsets, validated byte for byte against Skyrim.esm: Elenwen
// (0x00013269) reads level 30 in the range 0..30, Ahtar (0x0001325F) reads the
// multiplier 1000 in the range 6..30, and both match what the savegame's own
// copy of the same block decodes to.
constexpr size_t kAcbsLevel = 8;
constexpr size_t kAcbsCalcMin = 10;
constexpr size_t kAcbsCalcMax = 12;
constexpr size_t kAcbsSize = 24;

u16 ReadU16(const u8* data, size_t offset) {
  u16 value;
  std::memcpy(&value, data + offset, sizeof(value));
  return value;
}

}  // namespace

u32 ResolveActorLevel(u32 base_flags,
                      u32 stored_level,
                      u32 calc_min_level,
                      u32 calc_max_level,
                      u32 player_level) {
  if ((base_flags & kActorBaseFlagLevelMult) == 0)
    return stored_level == 0 ? 1 : stored_level;
  u32 level = player_level * stored_level / 1000;
  if (level < calc_min_level)
    level = calc_min_level;
  if (calc_max_level != 0 && level > calc_max_level)
    level = calc_max_level;
  return level == 0 ? 1 : level;
}

bool ReadActorStats(const Record& npc, u32 player_level, ActorStats* out) {
  const Subrecord* acbs = npc.Find(kAcbs);
  if (!acbs || acbs->data.size() < kAcbsSize)
    return false;

  ActorStats stats;
  u32 flags;
  std::memcpy(&flags, acbs->data.data(), sizeof(flags));
  stats.level = ResolveActorLevel(flags, ReadU16(acbs->data.data(), kAcbsLevel),
                                  ReadU16(acbs->data.data(), kAcbsCalcMin),
                                  ReadU16(acbs->data.data(), kAcbsCalcMax), player_level);

  if (const Subrecord* aidt = npc.Find(kAidt); aidt && aidt->data.size() >= 6) {
    const u8* d = aidt->data.data();
    stats.has_ai = true;
    stats.ai = {d[0], d[1], d[2], d[3], d[4], d[5]};
  }
  *out = stats;
  return true;
}

}  // namespace rx::bethesda
