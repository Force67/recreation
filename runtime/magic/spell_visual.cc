#include "runtime/magic/spell_visual.h"

namespace rx::magic {
namespace {

// One recipe per archetype, indexed by SpellArchetype. Values are tuned for the
// engine's HDR pipeline: core colours above 1 are deliberate, they are what the
// bloom pass turns into the glow the old shader had to paint by hand.
constexpr SpellVisual kVisuals[static_cast<u32>(SpellArchetype::kCount)] = {
    // kFire: white-hot core cooling through orange into smoke, rising and
    // curling. The only archetype with a smoke tail.
    {.core_color = {1.00f, 0.55f, 0.18f},
     .edge_color = {0.28f, 0.05f, 0.02f},
     .emissive = 2.4f,
     .light_color = {1.00f, 0.52f, 0.20f},
     .light_intensity = 9.0f,
     .light_radius = 12.0f,
     .flicker_hz = 11.7f,
     .flicker_depth = 0.26f,
     .spawn_rate = 260.0f,
     .life = 0.75f,
     .size = 0.055f,
     .spread = 0.40f,
     .speed = 1.6f,
     .buoyancy = 2.3f,
     .drag = 1.4f,
     .turbulence = 1.5f,
     .additive = true,
     .smoke_tail = true},
    // kFrost: pale blue shards that SINK and slow hard, drawn as alpha mist so
    // it reads as cold fog rather than glow.
    {.core_color = {0.62f, 0.86f, 1.00f},
     .edge_color = {0.14f, 0.28f, 0.45f},
     .emissive = 1.2f,
     .light_color = {0.42f, 0.68f, 1.00f},
     .light_intensity = 4.5f,
     .light_radius = 9.0f,
     .flicker_hz = 2.5f,
     .flicker_depth = 0.08f,
     .spawn_rate = 200.0f,
     .life = 0.95f,
     .size = 0.060f,
     .spread = 0.38f,
     .speed = 1.5f,
     .buoyancy = -0.9f,
     .drag = 2.6f,
     .turbulence = 0.5f,
     .additive = false,
     .smoke_tail = false},
    // kShock: near-white core with a violet edge, no buoyancy, heavy
    // turbulence and a fast strobe so the light snaps rather than breathes.
    {.core_color = {0.80f, 0.86f, 1.00f},
     .edge_color = {0.32f, 0.16f, 0.85f},
     .emissive = 3.0f,
     .light_color = {0.55f, 0.62f, 1.00f},
     .light_intensity = 14.0f,
     .light_radius = 14.0f,
     .flicker_hz = 30.0f,
     .flicker_depth = 0.60f,
     .spawn_rate = 300.0f,
     .life = 0.38f,
     .size = 0.040f,
     .spread = 0.55f,
     .speed = 3.0f,
     .buoyancy = 0.0f,
     .drag = 1.2f,
     .turbulence = 3.0f,
     .additive = true,
     .smoke_tail = false},
    // kRestoration: warm gold, gentle rise, steady light.
    {.core_color = {1.00f, 0.82f, 0.45f},
     .edge_color = {0.40f, 0.26f, 0.08f},
     .emissive = 1.8f,
     .light_color = {1.00f, 0.85f, 0.58f},
     .light_intensity = 6.0f,
     .light_radius = 10.0f,
     .flicker_hz = 3.0f,
     .flicker_depth = 0.10f,
     .spawn_rate = 180.0f,
     .life = 0.90f,
     .size = 0.050f,
     .spread = 0.38f,
     .speed = 1.2f,
     .buoyancy = 1.1f,
     .drag = 1.8f,
     .turbulence = 0.6f,
     .additive = true,
     .smoke_tail = false},
    // kIllusion: violet haze, alpha blended, barely lights anything.
    {.core_color = {0.72f, 0.45f, 1.00f},
     .edge_color = {0.20f, 0.10f, 0.35f},
     .emissive = 1.1f,
     .light_color = {0.60f, 0.40f, 1.00f},
     .light_intensity = 2.5f,
     .light_radius = 8.0f,
     .flicker_hz = 1.6f,
     .flicker_depth = 0.15f,
     .spawn_rate = 150.0f,
     .life = 1.10f,
     .size = 0.065f,
     .spread = 0.50f,
     .speed = 0.9f,
     .buoyancy = 0.3f,
     .drag = 2.2f,
     .turbulence = 0.9f,
     .additive = false,
     .smoke_tail = false},
    // kConjuration: deep purple-blue summoning glow, additive.
    {.core_color = {0.55f, 0.32f, 0.95f},
     .edge_color = {0.15f, 0.06f, 0.30f},
     .emissive = 2.0f,
     .light_color = {0.50f, 0.30f, 1.00f},
     .light_intensity = 7.0f,
     .light_radius = 11.0f,
     .flicker_hz = 5.0f,
     .flicker_depth = 0.20f,
     .spawn_rate = 190.0f,
     .life = 0.85f,
     .size = 0.055f,
     .spread = 0.42f,
     .speed = 1.4f,
     .buoyancy = 0.8f,
     .drag = 1.9f,
     .turbulence = 1.1f,
     .additive = true,
     .smoke_tail = false},
    // kAlteration: pale teal, calm and even.
    {.core_color = {0.50f, 0.88f, 0.76f},
     .edge_color = {0.12f, 0.25f, 0.22f},
     .emissive = 1.6f,
     .light_color = {0.50f, 0.85f, 0.75f},
     .light_intensity = 5.0f,
     .light_radius = 9.0f,
     .flicker_hz = 2.0f,
     .flicker_depth = 0.10f,
     .spawn_rate = 160.0f,
     .life = 0.95f,
     .size = 0.052f,
     .spread = 0.40f,
     .speed = 1.1f,
     .buoyancy = 0.6f,
     .drag = 2.0f,
     .turbulence = 0.7f,
     .additive = true,
     .smoke_tail = false},
    // kPoison: sickly green, sinks and lingers, alpha blended.
    {.core_color = {0.45f, 0.85f, 0.30f},
     .edge_color = {0.10f, 0.22f, 0.06f},
     .emissive = 1.0f,
     .light_color = {0.40f, 0.80f, 0.28f},
     .light_intensity = 3.0f,
     .light_radius = 7.0f,
     .flicker_hz = 1.8f,
     .flicker_depth = 0.12f,
     .spawn_rate = 140.0f,
     .life = 1.20f,
     .size = 0.062f,
     .spread = 0.48f,
     .speed = 0.8f,
     .buoyancy = -0.5f,
     .drag = 2.4f,
     .turbulence = 0.8f,
     .additive = false,
     .smoke_tail = false},
    // kDrain: dark red, pulled inward rather than thrown, alpha blended.
    {.core_color = {0.75f, 0.15f, 0.35f},
     .edge_color = {0.18f, 0.02f, 0.06f},
     .emissive = 1.3f,
     .light_color = {0.80f, 0.15f, 0.30f},
     .light_intensity = 4.0f,
     .light_radius = 8.0f,
     .flicker_hz = 4.0f,
     .flicker_depth = 0.22f,
     .spawn_rate = 160.0f,
     .life = 0.80f,
     .size = 0.050f,
     .spread = 0.38f,
     .speed = 1.0f,
     .buoyancy = 0.2f,
     .drag = 2.1f,
     .turbulence = 1.0f,
     .additive = false,
     .smoke_tail = false},
    // kGeneric: neutral magic wisp for effects that name no damage type.
    {.core_color = {0.78f, 0.84f, 1.00f},
     .edge_color = {0.20f, 0.22f, 0.30f},
     .emissive = 1.6f,
     .light_color = {0.70f, 0.78f, 1.00f},
     .light_intensity = 5.0f,
     .light_radius = 9.0f,
     .flicker_hz = 4.0f,
     .flicker_depth = 0.15f,
     .spawn_rate = 160.0f,
     .life = 0.90f,
     .size = 0.052f,
     .spread = 0.42f,
     .speed = 1.2f,
     .buoyancy = 0.5f,
     .drag = 2.0f,
     .turbulence = 0.9f,
     .additive = true,
     .smoke_tail = false},
};

// Case-insensitive substring test; the record text is not consistently cased.
bool Contains(base::StringRef haystack, const char* needle) {
  const size_t n = haystack.size();
  size_t len = 0;
  while (needle[len] != 0)
    ++len;
  if (len == 0 || len > n)
    return false;
  for (size_t i = 0; i + len <= n; ++i) {
    size_t j = 0;
    for (; j < len; ++j) {
      char a = haystack[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z')
        a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z')
        b = static_cast<char>(b - 'A' + 'a');
      if (a != b)
        break;
    }
    if (j == len)
      return true;
  }
  return false;
}

SpellArchetype ClassifyOne(base::StringRef text) {
  if (text.empty())
    return SpellArchetype::kCount;  // "no opinion"

  if (Contains(text, "fire") || Contains(text, "flame") || Contains(text, "burn") ||
      Contains(text, "incinerat") || Contains(text, "firebolt") || Contains(text, "fireball"))
    return SpellArchetype::kFire;
  if (Contains(text, "frost") || Contains(text, "ice") || Contains(text, "freeze") ||
      Contains(text, "cold") || Contains(text, "blizzard"))
    return SpellArchetype::kFrost;
  if (Contains(text, "shock") || Contains(text, "lightning") || Contains(text, "spark") ||
      Contains(text, "thunder") || Contains(text, "storm"))
    return SpellArchetype::kShock;
  // Poison and drain before restoration: "absorb health" is a drain, not a heal.
  if (Contains(text, "poison") || Contains(text, "venom"))
    return SpellArchetype::kPoison;
  if (Contains(text, "absorb") || Contains(text, "drain") || Contains(text, "leech"))
    return SpellArchetype::kDrain;
  if (Contains(text, "heal") || Contains(text, "restor") || Contains(text, "ward") ||
      Contains(text, "turnundead") || Contains(text, "sunfire"))
    return SpellArchetype::kRestoration;
  if (Contains(text, "conjur") || Contains(text, "summon") || Contains(text, "raise") ||
      Contains(text, "bound") || Contains(text, "atronach") || Contains(text, "soultrap") ||
      Contains(text, "reanimate"))
    return SpellArchetype::kConjuration;
  if (Contains(text, "illusion") || Contains(text, "fear") || Contains(text, "calm") ||
      Contains(text, "fury") || Contains(text, "frenzy") || Contains(text, "invisib") ||
      Contains(text, "muffle") || Contains(text, "courage"))
    return SpellArchetype::kIllusion;
  if (Contains(text, "alter") || Contains(text, "flesh") || Contains(text, "paralysis") ||
      Contains(text, "telekinesis") || Contains(text, "candlelight") ||
      Contains(text, "magelight") || Contains(text, "waterbreathing") ||
      Contains(text, "dragonhide"))
    return SpellArchetype::kAlteration;
  return SpellArchetype::kCount;
}

}  // namespace

const SpellVisual& VisualFor(SpellArchetype archetype) {
  const u32 index = static_cast<u32>(archetype);
  if (index >= static_cast<u32>(SpellArchetype::kCount))
    return kVisuals[static_cast<u32>(SpellArchetype::kGeneric)];
  return kVisuals[index];
}

const char* SpellArchetypeName(SpellArchetype archetype) {
  switch (archetype) {
    case SpellArchetype::kFire:
      return "fire";
    case SpellArchetype::kFrost:
      return "frost";
    case SpellArchetype::kShock:
      return "shock";
    case SpellArchetype::kRestoration:
      return "restoration";
    case SpellArchetype::kIllusion:
      return "illusion";
    case SpellArchetype::kConjuration:
      return "conjuration";
    case SpellArchetype::kAlteration:
      return "alteration";
    case SpellArchetype::kPoison:
      return "poison";
    case SpellArchetype::kDrain:
      return "drain";
    case SpellArchetype::kGeneric:
    case SpellArchetype::kCount:
      break;
  }
  return "generic";
}

SpellArchetype ClassifySpell(base::StringRef editor_id, base::StringRef keyword) {
  // The keyword is the more reliable of the two when an effect carries one
  // (MagicDamageFire and friends), so it gets the first say.
  SpellArchetype from_keyword = ClassifyOne(keyword);
  if (from_keyword != SpellArchetype::kCount)
    return from_keyword;
  SpellArchetype from_id = ClassifyOne(editor_id);
  if (from_id != SpellArchetype::kCount)
    return from_id;
  return SpellArchetype::kGeneric;
}

}  // namespace rx::magic
