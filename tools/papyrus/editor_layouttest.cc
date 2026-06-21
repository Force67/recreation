// editor_layouttest: deterministic checks for the map editor's layout file
// format (runtime/editor_layout.h). It round-trips placed-object records and
// confirms blanks, comments and malformed lines are rejected, so the
// save/load persistence format stays stable. Needs no game data, so it runs in
// the ctest gate.

#include <cmath>
#include <cstdio>
#include <string>

#include "editor_layout.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

bool Near(rec::f32 a, rec::f32 b) { return std::fabs(a - b) < 1e-4f; }

}  // namespace

int main() {
  using rec::editor::FormatPlaceLine;
  using rec::editor::LayoutEntry;
  using rec::editor::ParsePlaceLine;

  std::printf("editor layout format\n");

  // Round-trip a representative record (FO4-style plugin index + a real-ish id,
  // a non-trivial transform).
  LayoutEntry in;
  in.domain = "fallout4";
  in.base = rec::bethesda::GlobalFormId{4, 0x0123ab};
  in.pos[0] = -140.5f;
  in.pos[1] = 29.13f;
  in.pos[2] = 1191.28f;
  in.rot[0] = 0.0f;
  in.rot[1] = 0.7071f;
  in.rot[2] = 0.0f;
  in.rot[3] = 0.7071f;
  in.scale = 1.25f;

  const std::string line = FormatPlaceLine(in);
  Check("formats with the place tag", line.rfind("place ", 0) == 0);

  LayoutEntry out;
  Check("parses its own output", ParsePlaceLine(line, &out));
  Check("domain survives", out.domain == in.domain);
  Check("plugin survives", out.base.plugin == in.base.plugin);
  Check("local id survives", out.base.local_id == in.base.local_id);
  Check("position survives",
        Near(out.pos[0], in.pos[0]) && Near(out.pos[1], in.pos[1]) && Near(out.pos[2], in.pos[2]));
  Check("rotation survives", Near(out.rot[0], in.rot[0]) && Near(out.rot[1], in.rot[1]) &&
                                 Near(out.rot[2], in.rot[2]) && Near(out.rot[3], in.rot[3]));
  Check("scale survives", Near(out.scale, in.scale));

  // Non-records are skipped.
  LayoutEntry scratch;
  Check("rejects a blank line", !ParsePlaceLine("", &scratch));
  Check("rejects a comment", !ParsePlaceLine("# recreation map layout v1", &scratch));
  Check("rejects an unknown tag", !ParsePlaceLine("delete skyrimse 4 1 2 3", &scratch));
  Check("rejects a truncated record", !ParsePlaceLine("place skyrimse 4 1 2 3", &scratch));

  // A hand-written record parses to the right values (format contract).
  LayoutEntry hand;
  Check("parses a hand-written record",
        ParsePlaceLine("place skyrimse 0 305 1 2 3 0 0 0 1 2", &hand));
  Check("hand record values", hand.domain == "skyrimse" && hand.base.plugin == 0 &&
                                  hand.base.local_id == 305 && Near(hand.pos[0], 1) &&
                                  Near(hand.scale, 2));

  std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "passed", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
