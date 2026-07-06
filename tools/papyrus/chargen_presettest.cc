// chargen_presettest: deterministic checks for the character-creation preset
// file format (runtime/chargen_layout.h). It round-trips a saved face (race,
// sex, preset, skin/hair colours, NAMA type indices, sparse NAM9 sliders and
// layered chargen morphs) and confirms comments / blanks / unrelated text are
// rejected, so the save/load persistence format stays stable. Needs no game
// data, so it runs in the ctest gate (mirrors editor_layouttest).

#include <cmath>
#include <cstdio>
#include <string>

#include "chargen_layout.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

bool Near(rec::f32 a, rec::f32 b) { return std::fabs(a - b) < 1e-4f; }

}  // namespace

int main() {
  using rec::chargen::CharGenPreset;
  using rec::chargen::ParseCharGenPreset;
  using rec::chargen::SerializeCharGenPreset;

  std::printf("chargen preset format\n");

  // Round-trip a representative preset (a female Orc, a picked RPRM preset and
  // hairstyle, a warm skin tone, two type indices, two sliders and a morph).
  CharGenPreset in;
  in.race = "OrcRace";
  in.sex = 1;
  in.preset = 3;
  in.subdiv = 2;
  in.hair_style = 5;
  in.hair_color = 2;
  in.skin[0] = 0.55f;
  in.skin[1] = 0.42f;
  in.skin[2] = 0.33f;
  in.hair[0] = 0.12f;
  in.hair[1] = 0.09f;
  in.hair[2] = 0.07f;
  in.nama[0] = 7;
  in.nama[1] = -1;
  in.nama[2] = 12;
  in.nama[3] = 4;
  in.nam9.emplace_back(0, 0.35f);
  in.nam9.emplace_back(5, -0.8f);
  in.morphs.emplace_back("NoseLong", 0.75f);

  const std::string text = SerializeCharGenPreset(in);
  Check("serializes with the version header", text.rfind("# recreation chargen preset", 0) == 0);

  CharGenPreset out;
  Check("parses its own output", ParseCharGenPreset(text, &out));
  Check("race survives", out.race == in.race);
  Check("sex survives", out.sex == in.sex);
  Check("preset survives", out.preset == in.preset);
  Check("subdiv survives", out.subdiv == in.subdiv);
  Check("hairstyle survives", out.hair_style == in.hair_style);
  Check("haircolor survives", out.hair_color == in.hair_color);
  Check("skin survives",
        Near(out.skin[0], in.skin[0]) && Near(out.skin[1], in.skin[1]) && Near(out.skin[2], in.skin[2]));
  Check("hair colour survives",
        Near(out.hair[0], in.hair[0]) && Near(out.hair[1], in.hair[1]) && Near(out.hair[2], in.hair[2]));
  Check("nama survives", out.nama[0] == 7 && out.nama[1] == -1 && out.nama[2] == 12 && out.nama[3] == 4);
  Check("nam9 count survives", out.nam9.size() == 2);
  Check("nam9 values survive",
        out.nam9.size() == 2 && out.nam9[0].first == 0 && Near(out.nam9[0].second, 0.35f) &&
            out.nam9[1].first == 5 && Near(out.nam9[1].second, -0.8f));
  Check("morphs survive",
        out.morphs.size() == 1 && out.morphs[0].first == "NoseLong" && Near(out.morphs[0].second, 0.75f));

  // Non-presets are rejected (nothing recognized to read).
  CharGenPreset scratch;
  Check("rejects an empty document", !ParseCharGenPreset("", &scratch));
  Check("rejects a comment-only document", !ParseCharGenPreset("# recreation chargen preset v1\n", &scratch));
  Check("rejects unrelated text", !ParseCharGenPreset("hello world\nfoo bar\n", &scratch));

  // A hand-written document parses to the right values (format contract).
  CharGenPreset hand;
  Check("parses a hand-written document",
        ParseCharGenPreset("race BretonRace\nsex 0\nnam9 2 0.5\n", &hand));
  Check("hand document values",
        hand.race == "BretonRace" && hand.sex == 0 && hand.nam9.size() == 1 &&
            hand.nam9[0].first == 2 && Near(hand.nam9[0].second, 0.5f));

  std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "passed", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
