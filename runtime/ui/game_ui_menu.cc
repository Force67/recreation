// The front-of-house screens: character creation, the NEXUS main menu and the
// first-run wizard. All three are modal, own their own input, and are driven
// from engine-supplied views.

#include "runtime/ui/game_ui_internal.h"

#if defined(RECREATION_HAS_UGUI)

#include <ugui/svg/svg.h>  // the masthead wordmark is vector art, not set type

namespace rx {

base::String BuildCharGenSection() {
  const char* FLD = "#000000";  // sunken field
  const char* AC = "#ffffff";   // accent indigo
  const char* TXP = "#ffffff";  // primary text
  const char* TXS = "#9a9a9a";  // secondary text
  const char* TXM = "#5e5e5e";  // muted text
  base::String s;
  char buf[1400];

  const float contentW = kCgLeftW - 2 * kCgPad;
  const float sexW = (contentW - 8.0f) / 2.0f;
  const float tabW = (contentW - 2 * 6.0f) / 3.0f;

  std::snprintf(buf, sizeof(buf),
                "\n  panel cg_root {\n"
                "    position: absolute; top: 0; left: 0; width: 100vw; height: 100vh;\n"
                "    panel cg_left { position: absolute; left: 0; top: %g; width: %g; bottom: %g;"
                " background: #0d0d0dff; border-color: #ffffff12; border-width: 1;\n",
                kCgTop, kCgLeftW, kCgTop);
  s += buf;
  std::snprintf(buf, sizeof(buf),
                "      text { position: absolute; left: %g; top: 16; font-size: 16; color: %s;"
                " letter-spacing: 2; text: \"CHARACTER CREATION\"; }\n"
                "      text { position: absolute; left: %g; top: 60; font-size: 11; color: %s;"
                " letter-spacing: 1; text: \"RACE\"; }\n",
                kCgPad, TXP, kCgPad, TXM);
  s += buf;
  // Race rows (pooled).
  for (int i = 0; i < kCgRaceRows; ++i) {
    const base::String id = base::ToString(i);
    std::snprintf(buf, sizeof(buf),
                  "      panel cg_race%s { position: absolute; left: %g; top: %g; width: %g;"
                  " height: %g; background: #ffffff00; cursor: pointer;"
                  " :hover { background: #ffffff0e; }\n"
                  "        text cg_race%s_t { position: absolute; left: 10; top: 5; font-size: 12;"
                  " color: %s; text: \"\"; }\n      }\n",
                  id.c_str(), kCgPad, kCgRaceY0 + i * kCgRaceRowH, contentW, kCgRaceRowH - 3.0f,
                  id.c_str(), TXS);
    s += buf;
  }
  // Sex toggle.
  std::snprintf(buf, sizeof(buf),
                "      text { position: absolute; left: %g; top: %g; font-size: 11; color: %s;"
                " letter-spacing: 1; text: \"SEX\"; }\n",
                kCgPad, kCgSexY - 24.0f, TXM);
  s += buf;
  std::snprintf(
      buf, sizeof(buf),
      "      panel cg_sexm { position: absolute; left: %g; top: %g; width: %g; height: %g;"
      " background: %s; border-color: #ffffff14; border-width: 1;"
      " layout: column; justify: center; align: center; cursor: pointer;"
      " text cg_sexm_t { font-size: 13; color: %s; text: \"Male\"; } }\n"
      "      panel cg_sexf { position: absolute; left: %g; top: %g; width: %g; height: %g;"
      " background: %s; border-color: #ffffff14; border-width: 1;"
      " layout: column; justify: center; align: center; cursor: pointer;"
      " text cg_sexf_t { font-size: 13; color: %s; text: \"Female\"; } }\n",
      kCgPad, kCgSexY, sexW, kCgBtnH, FLD, TXP, kCgPad + sexW + 8.0f, kCgSexY, sexW, kCgBtnH, FLD,
      TXS);
  s += buf;
  // Preset cycler.
  std::snprintf(buf, sizeof(buf),
                "      text { position: absolute; left: %g; top: %g; font-size: 11; color: %s;"
                " letter-spacing: 1; text: \"PRESET\"; }\n",
                kCgPad, kCgPresetY - 24.0f, TXM);
  s += buf;
  std::snprintf(
      buf, sizeof(buf),
      "      panel cg_pprev { position: absolute; left: %g; top: %g; width: 40; height: %g;"
      " background: %s; layout: column; justify: center; align: center;"
      " cursor: pointer; :hover { background: #ffffff18; }"
      " text { font-size: 15; color: %s; text: \"<\"; } }\n"
      "      text cg_plabel { position: absolute; left: %g; top: %g; width: %g;"
      " text-align: center; font-size: 12; color: %s; text: \"Preset 1\"; }\n"
      "      panel cg_pnext { position: absolute; left: %g; top: %g; width: 40; height: %g;"
      " background: %s; layout: column; justify: center; align: center;"
      " cursor: pointer; :hover { background: #ffffff18; }"
      " text { font-size: 15; color: %s; text: \">\"; } }\n",
      kCgPad, kCgPresetY, kCgBtnH, FLD, TXP, kCgPad + 46.0f, kCgPresetY + 8.0f, contentW - 92.0f,
      TXP, kCgPad + contentW - 40.0f, kCgPresetY, kCgBtnH, FLD, TXP);
  s += buf;
  // Page tabs.
  const char* pages[3] = {"Face", "Advanced", "Look"};
  for (int i = 0; i < 3; ++i) {
    const base::String id = base::ToString(i);
    std::snprintf(buf, sizeof(buf),
                  "      panel cg_page%s { position: absolute; left: %g; top: %g; width: %g;"
                  " height: 32; background: %s; layout: column; justify: center;"
                  " align: center; cursor: pointer;"
                  " text cg_page%s_t { font-size: 12; color: %s; text: \"%s\"; } }\n",
                  id.c_str(), kCgPad + i * (tabW + 6.0f), kCgPageY, tabW, i == 0 ? AC : FLD,
                  id.c_str(), i == 0 ? TXP : TXS, pages[i]);
    s += buf;
  }
  // Action buttons.
  const char* acts[3] = {"Randomize", "Reset", "Save"};
  const char* actn[3] = {"cg_rand", "cg_reset", "cg_save"};
  for (int i = 0; i < 3; ++i) {
    std::snprintf(buf, sizeof(buf),
                  "      panel %s { position: absolute; left: %g; top: %g; width: %g; height: 30;"
                  " background: %s; border-color: #ffffff14; border-width: 1;"
                  " layout: column; justify: center; align: center; cursor: pointer;"
                  " :hover { background: #ffffff14; }"
                  " text { font-size: 13; color: %s; text: \"%s\"; } }\n",
                  actn[i], kCgPad, kCgActY + i * kCgActH, contentW, i == 2 ? AC : FLD,
                  i == 2 ? TXP : TXS, acts[i]);
    s += buf;
  }
  std::snprintf(buf, sizeof(buf),
                "      text cg_status { position: absolute; left: %g; top: %g; width: %g;"
                " font-size: 11; color: %s; text: \"\"; }\n    }\n",
                kCgPad, kCgActY + 3 * kCgActH + 8.0f, contentW, TXS);
  s += buf;

  // Right dock: page title + pager + pooled control rows.
  std::snprintf(
      buf, sizeof(buf),
      "    panel cg_right { position: absolute; right: 0; top: %g; width: %g; bottom: %g;"
      " background: #0d0d0dff; border-color: #ffffff12; border-width: 1;\n"
      "      text cg_ptitle { position: absolute; left: 16; top: 16; font-size: 15;"
      " color: %s; letter-spacing: 1; text: \"FACE\"; }\n"
      "      text cg_scinfo { position: absolute; left: 16; top: 40; font-size: 11;"
      " color: %s; text: \"\"; }\n"
      "      panel cg_scup { position: absolute; right: 52; top: 14; width: 30; height: 26;"
      " background: %s; layout: column; justify: center; align: center;"
      " cursor: pointer; :hover { background: #ffffff18; }"
      " text { font-size: 13; color: %s; text: \"^\"; } }\n"
      "      panel cg_scdn { position: absolute; right: 16; top: 14; width: 30; height: 26;"
      " background: %s; layout: column; justify: center; align: center;"
      " cursor: pointer; :hover { background: #ffffff18; }"
      " text { font-size: 13; color: %s; text: \"v\"; } }\n",
      kCgTop, kCgRightW, kCgTop, TXP, TXM, FLD, TXP, FLD, TXP);
  s += buf;
  for (int i = 0; i < kCgSliderRows; ++i) {
    const base::String id = base::ToString(i);
    std::snprintf(
        buf, sizeof(buf),
        "      panel cg_row%s { position: absolute; left: 8; top: %g; width: %g;"
        " height: %g; background: #ffffff00; cursor: pointer;"
        " :hover { background: #ffffff0c; }\n"
        "        text cg_row%s_lbl { position: absolute; left: 10; top: 9; font-size: 12;"
        " color: #c9cfdb; text: \"\"; }\n"
        "        panel cg_row%s_sw { position: absolute; left: %g; top: 8; width: 14;"
        " height: 14; background: #000000; }\n"
        "        panel cg_row%s_trk { position: absolute; left: %g; top: %g; width: %g;"
        " height: 8; background: %s; border-color: #ffffff14;"
        " border-width: 1;\n"
        "          panel cg_row%s_fill { position: absolute; left: 0; top: 0; width: 50%%;"
        " height: 8; background: %s; }\n        }\n"
        "        text cg_row%s_val { position: absolute; left: %g; top: 9; font-size: 11;"
        " color: %s; text: \"\"; }\n      }\n",
        id.c_str(), kCgRowsY0 + i * kCgRowH, kCgRightW - 16.0f, kCgRowH - 6.0f, id.c_str(),
        id.c_str(), kCgTrackX - 8.0f - 20.0f, id.c_str(), kCgTrackX - 8.0f,
        (kCgRowH - 6.0f) / 2.0f - 4.0f, kCgTrackW, FLD, id.c_str(), AC, id.c_str(),
        kCgTrackX - 8.0f + kCgTrackW + 10.0f, TXS);
    s += buf;
  }
  s += "    }\n  }\n";
  return s;
}

// --- UI markup fragments (runtime/ui/*.ugui) --------------------------------
// The static screens live in editable .ugui files so they can be tweaked and
// hot-reloaded without a rebuild. The procedural screens (the scrolling compass
// topbar, the map editor, the NEXUS main menu) are generated in code and
// concatenated in as siblings of root. See RECREATION_UI_DIR (override the
// fragment directory) and RECREATION_UI_HOT_RELOAD (reload on file change).

// The .ugui fragments composed into root, in draw order. Also the hot-reload
// watch list.

void GameUi::Impl::ApplyCharGenView() {
  // On the active<->inactive edge, hide the gameplay HUD while creating a
  // character and restore it on exit (the overlay owns the whole screen).
  if (chargen.active != chargen_prev_active) {
    const bool hud = !chargen.active && !UsingVanillaUi();
    SetVisible("topbar", hud);
    SetVisible("crosshair", hud);
    SetVisible("vitals", hud);
    SetVisible("readout", hud);
    chargen_prev_active = chargen.active;
  }
  SetVisible("cg_root", chargen.active);
  if (!chargen.active)
    return;

  auto setText = [&](const base::String& n, const base::String& t) {
    SetText(n.c_str(), t.c_str());
  };
  auto setFill = [&](const base::String& n, float pct) {
    SetStyleField(n.c_str(), [](ugui::Style& s, float v) { s.width = ugui::Length::Pct(v); }, pct);
  };

  // Race list.
  for (int i = 0; i < kCgRaceRows; ++i) {
    const base::String row = "cg_race" + base::ToString(i);
    if (i < static_cast<int>(chargen.races.size())) {
      SetVisible(row.c_str(), true);
      setText(row + "_t", chargen.races[i]);
      const bool on = i == chargen.race;
      SetBackground(row.c_str(), on ? kEdAccentSoft : kEdClear);
      SetTextColor((row + "_t").c_str(), on ? kEdTxP : kEdTxS);
    } else {
      SetVisible(row.c_str(), false);
    }
  }

  // Sex toggle + page tabs.
  SetBackground("cg_sexm", chargen.sex == 0 ? kEdAccent : kEdField);
  SetBackground("cg_sexf", chargen.sex == 1 ? kEdAccent : kEdField);
  SetTextColor("cg_sexm_t", chargen.sex == 0 ? kEdTxP : kEdTxS);
  SetTextColor("cg_sexf_t", chargen.sex == 1 ? kEdTxP : kEdTxS);
  for (int i = 0; i < 3; ++i) {
    const base::String tab = "cg_page" + base::ToString(i);
    const bool on = i == chargen.page;
    SetBackground(tab.c_str(), on ? kEdAccent : kEdField);
    SetTextColor((tab + "_t").c_str(), on ? kEdTxP : kEdTxS);
  }
  setText("cg_plabel", chargen.preset_label);
  setText("cg_status", chargen.status);

  // Right dock: page title, pager readout, control rows.
  setText("cg_ptitle", chargen.page_title);
  if (chargen.row_total > kCgSliderRows) {
    char b[64];
    std::snprintf(b, sizeof(b), "%d-%d / %d", chargen.row_first + 1,
                  base::Min(chargen.row_first + kCgSliderRows, chargen.row_total),
                  chargen.row_total);
    setText("cg_scinfo", b);
  } else {
    setText("cg_scinfo", "");
  }
  for (int i = 0; i < kCgSliderRows; ++i) {
    const base::String row = "cg_row" + base::ToString(i);
    if (i < static_cast<int>(chargen.rows.size())) {
      const CharGenView::Row& r = chargen.rows[i];
      SetVisible(row.c_str(), true);
      setText(row + "_lbl", r.label);
      setText(row + "_val", r.value);
      setFill(row + "_fill", base::Clamp(r.fill, 0.0f, 1.0f) * 100.0f);
      const bool sw = r.swatch != 0;
      SetVisible((row + "_sw").c_str(), sw);
      if (sw)
        SetBackground((row + "_sw").c_str(), Rgba(r.swatch));
    } else {
      SetVisible(row.c_str(), false);
    }
  }
}


// Until the engine pushes a real grid, the tiles are the located universes, so
// the menu is playable on the old three-column data path.
void GameUi::Impl::RebuildDerivedEntries() {
  if (mm_entries_pushed)
    return;
  mm_entries.clear();
  for (int i = 0; i < static_cast<int>(mm_universe_names.size()); ++i) {
    GameUi::MenuEntry e;
    e.title = mm_universe_names[i];
    e.universe = i;
    e.available = i >= static_cast<int>(mm_available.size()) || mm_available[i];
    e.state = e.available ? "Ready" : "Not located";
    e.art = i < kMenuUniverses ? mm_backdrop[i] : 0;
    mm_entries.push_back(base::move(e));
  }
  mm_entry = base::Clamp(mm_entry, 0, base::Max(0, static_cast<int>(mm_entries.size()) - 1));
}

void GameUi::Impl::ApplyMainMenu() {
  // A translated boot menu replaces the host's front screen rather than sitting
  // on top of it; the two would otherwise both take the same clicks.
  SetVisible("mainmenu", main_menu_open && !VanillaCovers(VanillaRole::kFrontMenu));
  ApplyVanillaVisibility();
  if (!main_menu_open) {
    mm_prev_open = false;
    return;
  }
  mm_prev_open = true;
  auto setText = [&](const base::String& n, const base::String& t) {
    SetText(n.c_str(), t.c_str());
  };

  // The masthead wordmark, rasterized the first time the screen comes up and
  // then carried by the emblem rebind below. Twice the 266x32 box the markup
  // lays out, so it holds up where the front screens are scaled past 1:1.
  if (!mm_wordmark_tried) {
    mm_wordmark_tried = true;
    const fs::path svg = UiDir() / "recreation_wordmark.svg";
    const ugui::TextureId tex = ugui::LoadSvgTexture(&backend, svg.string().c_str(), 532, 64);
    if (tex == ugui::kNullTextureId)
      RX_WARN("ui: cannot rasterize the wordmark: {}", svg.string());
    else
      mm_glyphs.emplace_back("mm_wordmark", tex);
  }

  // Emblems: rebind each frame so they survive a hot-reload tree rebuild. The
  // targets are optional (a screen may not carry that mark), hence FindWidget.
  for (const auto& [name, tex] : mm_glyphs) {
    ugui::wid w = tex ? ui.FindWidget(name.c_str()) : ugui::wid{};
    if (w.valid())
      ugui::SetImageTexture(w, tex, 1.0f, 1.0f);
  }

  // The tile grid: this page's entries, everything else collapsed. Selection is
  // stated by the top rule, the border lift, and the art coming up to full.
  const int count = static_cast<int>(mm_entries.size());
  mm_entry = base::Clamp(mm_entry, 0, base::Max(0, count - 1));
  const int page = mm_page();
  for (int i = 0; i < kMenuTiles; ++i) {
    const int index = page * kMenuTiles + i;
    const bool shown = index < count;
    SetVisible(Pooled("mm_tile", i), shown);
    if (!shown)
      continue;
    const GameUi::MenuEntry& e = mm_entries[index];
    const bool on = index == mm_entry;
    const bool avail = e.available;

    const bool has_art = e.art != 0;
    if (has_art)
      ugui::SetImageTexture(Need(Pooled("mm_art", i)), e.art, 1.0f, 1.0f);
    SetVisible(Pooled("mm_art", i), has_art);
    SetStyleField(Pooled("mm_art", i), [](ugui::Style& s, float v) { s.opacity = v; },
                  on ? 1.0f : (avail ? 0.62f : 0.14f));
    SetVisible(Pooled("mm_top", i), on);
    SetBorderColor(Pooled("mm_tile", i), Rgba(on ? 0xffffff8cu : 0xffffff1cu));

    // Load-order spine: one tick per plugin bucket the entry reports.
    const int ticks = base::Clamp(e.plugins, 0, kMenuSpineTicks);
    for (int t = 0; t < kMenuSpineTicks; ++t) {
      const char* tick = Pooled("mm_sp", i * kMenuSpineTicks + t);
      SetVisible(tick, t < ticks);
      if (t < ticks)
        SetBackground(tick, Rgba(on ? 0xffffffccu : 0xffffff3du));
    }

    SetText(Pooled("mm_state", i), e.state);
    SetTextColor(Pooled("mm_state", i),
                 Rgba(on ? 0xffffffffu : (avail ? 0xffffff8cu : 0x5e5e5effu)));
    // A mode names the world it is mounted on, so the two read at one weight.
    SetText(Pooled("mm_kind", i),
            e.kind == GameUi::MenuEntry::Kind::kMode
                ? (e.domain.empty() ? base::String("Mode") : "Mode  ·  " + e.domain)
                : base::String("Game"));
    SetText(Pooled("mm_title", i), e.title);
    SetTextColor(Pooled("mm_title", i),
                 Rgba(on ? 0xffffffffu : (avail ? 0xffffffccu : 0x5e5e5effu)));
    SetText(Pooled("mm_detail", i), e.detail);

    // The play button is glass at rest and solid white when focused, so the one
    // key that boots is always the brightest thing on the screen.
    SetBackground(Pooled("mm_play", i),
                  Rgba(on ? 0xffffffffu : (avail ? 0x060606ffu : 0x030303ffu)));
    SetBorderColor(Pooled("mm_play", i),
                   Rgba(on ? 0xffffffffu : (avail ? 0xffffff33u : 0xffffff14u)));
    SetBackground(Pooled("mm_tri", i),
                  Rgba(on ? 0x000000ffu : (avail ? 0xffffffccu : 0x3a3a3affu)));
  }

  // Pager: the readout and one pip per page, the current one lit.
  const int pages = mm_pages();
  setText("mm_pagelbl", "Page " + base::ToString(page + 1) + " / " + base::ToString(pages));
  for (int i = 0; i < kMenuPips; ++i) {
    SetVisible(Pooled("mm_pip", i), i < pages);
    if (i < pages)
      SetBackground(Pooled("mm_pip", i), Rgba(i == page ? 0xffffffffu : 0xffffff26u));
  }

  // Session segment: the live member takes the rule and full white, the other
  // two sit at the disabled value. It governs the grid and the rail below it,
  // which is why it sits in their header and not in the nav. The header rides
  // above the sub-screen overlay, so it goes down with the grid.
  // The grid and its header sit above mm_screen's top edge, so a sub-screen
  // takes them down rather than half-covering them.
  SetVisible("mm_playrow", mm_screen == 0);
  SetVisible("mm_grid", mm_screen == 0);
  for (int i = 0; i < 3; ++i) {
    const bool on = static_cast<int>(mm_session) == i;
    const base::String id = base::ToString(i);
    SetTextColor(("mm_sess" + id + "_t").c_str(), Rgba(on ? 0xffffffffu : 0x5e5e5effu));
    SetVisible(("mm_sess" + id + "_r").c_str(), on);
  }
  // The footer used to state the session as one word. It now carries the live
  // one, and says nothing at all when there is nothing hosted.
  setText("mm_sess_line", mm_session_line);

  ApplyMenuRail();

  // Profile banner: real handle + system line; peer count only when in session.
  const base::String sysline =
      mm_stats.in_game && !mm_stats.universe.empty()
          ? ("Level " + base::ToString(mm_stats.level) + "  ·  " + mm_stats.universe)
          : (mm_stats.account + (mm_stats.machine.empty() ? "" : "@" + mm_stats.machine));
  setText("mm_pname", mm_stats.player_name.empty() ? mm_stats.account : mm_stats.player_name);
  setText("mm_psys", sysline);
  SetVisible("mm_pnet", mm_stats.players_online > 0);
  setText("mm_pcount", base::ToString(mm_stats.players_online));

  // The guided demo in the nav: shown only when one is staged, dimmed to the
  // disabled value when its world was never located (it is still worth seeing
  // the entry, so a player knows the tour exists and why it cannot run).
  SetVisible("mm_util_tour", !mm_tour_title.empty());
  if (!mm_tour_title.empty()) {
    setText("mm_util_tour_t", mm_tour_title);
    SetTextColor("mm_util_tour_t", Rgba(mm_tour_available ? 0xffffffffu : 0xffffff3du));
  }

  // Build/version stamp.
  setText("mm_build", mm_stats.build.empty() ? "" : ("v" + mm_stats.build));

  // Sub-screen overlay: title + which body is shown. Body visibility is set
  // unconditionally (not only when the screen is open) so a body never lingers
  // visible while its screen is collapsed.
  SetVisible("mm_screen", mm_screen != 0);
  SetVisible("mm_body_mp", mm_screen == 1);
  SetVisible("mm_body_mods", mm_screen == 2);
  SetVisible("mm_body_settings", mm_screen == 3);
  SetVisible("mm_body_profile", mm_screen == 4);
  SetVisible("mm_body_load", mm_screen == 5);
  SetVisible("mm_body_join", mm_screen == 6);
  if (mm_screen != 0) {
    const char* titles[7] = {"",         "MULTIPLAYER", "MODS", "SETTINGS",
                             "PROFILE",  "LOAD",        "JOIN"};
    setText("mm_screen_title", titles[mm_screen]);
  }
  if (mm_screen == 5)
    ApplyMenuLoad();
  if (mm_screen == 6)
    ApplyMenuJoin();
  if (mm_screen == 1) {
    const int universe = mm_entry < count ? mm_entries[mm_entry].universe : 0;
    setText("mm_mp_universe", universe < static_cast<int>(mm_universe_names.size())
                                  ? mm_universe_names[universe]
                                  : "");
    setText("mm_mp_status", mm_stats.net_status.empty() ? "Offline" : mm_stats.net_status);
  }
  if (mm_screen == 2) {
    for (int i = 0; i < kMenuModRows; ++i) {
      const base::String id = base::ToString(i);
      const bool row = i < static_cast<int>(mm_mods.size());
      SetVisible(Pooled("mm_mod", i), row);
      if (row)
        setText("mm_modt" + id, mm_mods[i]);
    }
    SetVisible("mm_mods_empty", mm_mods.empty());
  }
  if (mm_screen == 4) {
    // Header + real account/system identity (always shown).
    setText("mm_pf_name", mm_stats.player_name.empty() ? mm_stats.account : mm_stats.player_name);
    setText("mm_pf_sub", mm_stats.in_game && !mm_stats.universe.empty()
                             ? ("Playing  ·  " + mm_stats.universe)
                             : "Local profile");
    setText("mm_pf_account", mm_stats.account.empty() ? "-" : mm_stats.account);
    setText("mm_pf_machine", mm_stats.machine.empty() ? "-" : mm_stats.machine);
    const base::String session =
        mm_stats.players_online > 0
            ? (base::ToString(mm_stats.players_online) + " online")
            : (mm_stats.net_status.empty() ? "Single-player" : mm_stats.net_status);
    setText("mm_pf_session", session);
    setText("mm_pf_build", mm_stats.build.empty() ? "-" : ("v" + mm_stats.build));

    // Character vitals/holdings only when a universe is actually loaded.
    SetVisible("mm_pf_char", mm_stats.in_game);
    SetVisible("mm_pf_hint", !mm_stats.in_game);
    if (mm_stats.in_game) {
      auto bar = [&](const char* fill, float v) {
        SetStyleField(
            fill, [](ugui::Style& s, float x) { s.width = ugui::Length::Pct(x); },
            base::Clamp(v, 0.0f, 1.0f) * 100.0f);
      };
      bar("mm_pf_health", mm_stats.health);
      bar("mm_pf_magicka", mm_stats.magicka);
      bar("mm_pf_stamina", mm_stats.stamina);
      setText("mm_pf_gold", base::ToString(mm_stats.gold));
      setText("mm_pf_quests", base::ToString(mm_stats.active_quests));
      setText("mm_pf_loc", mm_stats.location.empty() ? "-" : mm_stats.location);
    }
  }
}

// ---------------------------------------------------------------------------
// Savegames and sessions: the rail under the grid, the Load screen behind it,
// and the Join screen. The menu owns which row is selected and which tab is up;
// the engine only ever pushes the lists.

int GameUi::Impl::FocusedUniverse() const {
  if (mm_entry >= 0 && mm_entry < static_cast<int>(mm_entries.size()))
    return mm_entries[mm_entry].universe;
  return 0;
}

base::Vector<int> GameUi::Impl::SavesForUniverse(int universe) const {
  base::Vector<int> out;
  for (int i = 0; i < static_cast<int>(mm_saves.size()); ++i)
    if (mm_saves[i].universe == universe)
      out.push_back(i);
  return out;
}

// Only worlds that actually hold a save get a tab, so tab 0 is not always
// Skyrim and an empty world never offers an empty list.
base::Vector<int> GameUi::Impl::LoadTabUniverses() const {
  base::Vector<int> out;
  for (const GameUi::MenuSave& save : mm_saves) {
    bool seen = false;
    for (int universe : out)
      if (universe == save.universe) {
        seen = true;
        break;
      }
    if (!seen)
      out.push_back(save.universe);
  }
  return out;
}

base::Vector<int> GameUi::Impl::SavesForTab() const {
  const base::Vector<int> tabs = LoadTabUniverses();
  if (tabs.empty())
    return {};
  const int tab = base::Clamp(mm_load_tab, 0, static_cast<int>(tabs.size()) - 1);
  return SavesForUniverse(tabs[tab]);
}

base::Vector<int> GameUi::Impl::ServersForTab() const {
  base::Vector<int> out;
  for (int i = 0; i < static_cast<int>(mm_servers.size()); ++i) {
    // Friends is the only cut the menu can make on its own. Favourites and
    // history live in the managed browser model, and Direct is a dial rather
    // than a list, so both arrive already filtered.
    if (mm_join_tab == 1 && mm_servers[i].entry != "Friend")
      continue;
    out.push_back(i);
  }
  return out;
}

bool GameUi::Impl::DoubleClicked(const base::String& key) {
  const bool paired = key == mm_click_key && (ui_time - mm_click_time) <= kMenuDoubleClickSecs;
  mm_click_key = key;
  // Reset the clock on a pair, so a third click starts a new one instead of
  // firing again on the way past.
  mm_click_time = paired ? -1000.0f : ui_time;
  return paired;
}

void GameUi::Impl::ResumeSave(int save) {
  if (save < 0 || save >= static_cast<int>(mm_saves.size()))
    return;
  const GameUi::MenuSave& s = mm_saves[save];
  if (!s.loadable || s.path.empty())
    return;
  mm_request.kind = MainMenuRequest::Kind::kResumeSave;
  mm_request.universe = s.universe;
  mm_request.save_path = s.path;
  mm_request.session = mm_session;
  mm_request.multiplayer = mm_session != MenuSession::kSolo;
}

void GameUi::Impl::JoinServer(int server) {
  if (server < 0 || server >= static_cast<int>(mm_servers.size()))
    return;
  const GameUi::MenuServer& s = mm_servers[server];
  if (!s.joinable || s.address.empty())
    return;
  // Joining reads someone else's session, so the segment above the grid has no
  // say here: the host decides what this is.
  mm_request.kind = MainMenuRequest::Kind::kJoinServer;
  mm_request.universe = s.universe;
  mm_request.address = s.address;
  mm_request.multiplayer = true;
}

void GameUi::Impl::ApplyMenuRail() {
  const int universe = FocusedUniverse();
  const base::Vector<int> rail = SavesForUniverse(universe);
  // A world with nothing to continue collapses the rail whole, and the front
  // screen is the grid it has always been.
  SetVisible("mm_rail", !rail.empty() && mm_screen == 0);
  if (rail.empty()) {
    mm_rail = -1;
    return;
  }
  const int shown_cards =
      base::Min(static_cast<int>(rail.size()), static_cast<int>(kMenuRailCards));
  mm_rail = base::Clamp(mm_rail, -1, shown_cards - 1);

  const base::String world = universe < static_cast<int>(mm_universe_names.size())
                                 ? mm_universe_names[universe]
                                 : base::String();
  SetText("mm_rail_head", world.empty() ? base::String("Continue") : "Continue in " + world);
  const GameUi::MenuSave& newest = mm_saves[rail[0]];
  SetText("mm_rail_sub",
          newest.character + "  ·  level " + newest.level + "  ·  " + newest.played);
  SetText("mm_rail_count",
          base::ToString(static_cast<int>(rail.size())) + (rail.size() == 1 ? " save" : " saves"));

  for (int i = 0; i < kMenuRailCards; ++i) {
    const bool shown = i < static_cast<int>(rail.size());
    SetVisible(Pooled("mm_sv", i), shown);
    if (!shown)
      continue;
    const GameUi::MenuSave& s = mm_saves[rail[i]];
    const bool on = i == mm_rail;
    const base::String id = base::ToString(i);
    const base::String art = "mm_sv" + id + "_art";

    const bool has_art = s.art != 0;
    if (has_art)
      ugui::SetImageTexture(Need(art.c_str()), s.art, 1.0f, 1.0f);
    SetVisible(art.c_str(), has_art);
    SetVisible(("mm_sv" + id + "_top").c_str(), on);
    SetBorderColor(Pooled("mm_sv", i), Rgba(on ? 0xffffff8cu : 0xffffff1cu));
    SetText(("mm_sv" + id + "_when").c_str(),
            s.kind.empty() ? s.when : (s.kind + "  ·  " + s.when));
    SetTextColor(("mm_sv" + id + "_when").c_str(), Rgba(on ? 0xffffffffu : 0xffffff8cu));
    SetText(("mm_sv" + id + "_no").c_str(), s.slot);
    SetText(("mm_sv" + id + "_loc").c_str(), s.location);
    SetTextColor(("mm_sv" + id + "_loc").c_str(), Rgba(on ? 0xffffffffu : 0xffffffccu));
    // A save that will not resume as it is says so where the stats would go:
    // the number it replaces is the one thing that stops being interesting.
    SetText(("mm_sv" + id + "_meta").c_str(),
            s.loadable ? ("Level " + s.level + "  ·  " + s.played) : s.verdict);
    SetTextColor(("mm_sv" + id + "_meta").c_str(), Rgba(s.loadable ? 0xffffff7au : 0xffffffccu));
  }
}

void GameUi::Impl::ApplyMenuLoad() {
  const base::Vector<int> tabs = LoadTabUniverses();
  if (!tabs.empty())
    mm_load_tab = base::Clamp(mm_load_tab, 0, static_cast<int>(tabs.size()) - 1);
  for (int i = 0; i < kMenuUniverses; ++i) {
    const base::String id = base::ToString(i);
    const bool shown = i < static_cast<int>(tabs.size());
    SetVisible(("mm_ld_tab" + id).c_str(), shown);
    if (!shown)
      continue;
    const int universe = tabs[i];
    SetText(("mm_ld_tab" + id + "_t").c_str(),
            universe < static_cast<int>(mm_universe_names.size()) ? mm_universe_names[universe]
                                                                  : base::String("World"));
    const bool on = i == mm_load_tab;
    SetTextColor(("mm_ld_tab" + id + "_t").c_str(), Rgba(on ? 0xffffffffu : 0x5e5e5effu));
    SetVisible(("mm_ld_tab" + id + "_r").c_str(), on);
  }

  const base::Vector<int> rows = SavesForTab();
  const int total = static_cast<int>(rows.size());
  SetText("mm_ld_count", total == 0 ? base::String()
                                    : base::ToString(total) + (total == 1 ? " save" : " saves"));
  SetVisible("mm_ld_empty", total == 0);
  SetVisible("mm_ld_card", total > 0);

  // A selection from another tab is not a selection here.
  bool selected_here = false;
  for (int index : rows)
    if (index == mm_save) {
      selected_here = true;
      break;
    }
  if (!selected_here)
    mm_save = total > 0 ? rows[0] : -1;

  // Scroll the window rather than page it, so arrowing down a long list never
  // jumps the rows the eye is following.
  int selected_row = -1;
  for (int i = 0; i < total; ++i)
    if (rows[i] == mm_save) {
      selected_row = i;
      break;
    }
  mm_load_top = base::Clamp(mm_load_top, 0, base::Max(0, total - kMenuListRows));
  if (selected_row >= 0) {
    if (selected_row < mm_load_top)
      mm_load_top = selected_row;
    if (selected_row >= mm_load_top + kMenuListRows)
      mm_load_top = selected_row - kMenuListRows + 1;
  }

  int unloadable = 0;
  for (int index : rows)
    if (!mm_saves[index].loadable)
      ++unloadable;
  SetVisible("mm_ld_statusrow", unloadable > 0);
  if (unloadable > 0)
    SetText("mm_ld_status",
            base::ToString(unloadable) +
                (unloadable == 1 ? " save needs attention" : " saves need attention"));

  for (int i = 0; i < kMenuListRows; ++i) {
    const int at = mm_load_top + i;
    const bool shown = at < total;
    SetVisible(Pooled("mm_ld", i), shown);
    if (!shown)
      continue;
    const GameUi::MenuSave& s = mm_saves[rows[at]];
    const bool on = rows[at] == mm_save;
    const base::String id = base::ToString(i);
    const u32 body = s.loadable ? (on ? 0xffffffffu : 0xffffffd6u) : 0x5e5e5effu;
    const u32 dim = s.loadable ? 0x9a9a9affu : 0x4a4a4affu;

    SetBackground(Pooled("mm_ld", i), Rgba(on ? 0xffffff0au : 0x00000000u));
    // Filled pip resumes as it is, hairline wants attention. Value, not hue.
    SetBackground(("mm_ld" + id + "_pip").c_str(),
                  Rgba(s.loadable ? 0xffffffffu : 0x00000000u));
    SetBorderColor(("mm_ld" + id + "_pip").c_str(),
                   Rgba(s.loadable ? 0x00000000u : 0xffffff5eu));
    SetStyleField(
        ("mm_ld" + id + "_pip").c_str(),
        [](ugui::Style& style, float v) { style.border_width = v; }, s.loadable ? 0.0f : 1.0f);
    SetText(("mm_ld" + id + "_no").c_str(), s.slot);
    SetText(("mm_ld" + id + "_name").c_str(),
            s.kind.empty() ? s.character : (s.character + "   " + s.kind));
    SetTextColor(("mm_ld" + id + "_name").c_str(), Rgba(body));
    SetText(("mm_ld" + id + "_lvl").c_str(), s.level);
    SetText(("mm_ld" + id + "_loc").c_str(), s.location);
    SetTextColor(("mm_ld" + id + "_loc").c_str(), Rgba(on ? 0xffffffffu : dim));
    SetText(("mm_ld" + id + "_play").c_str(), s.played);
    SetText(("mm_ld" + id + "_when").c_str(), s.when);
    SetText(("mm_ld" + id + "_state").c_str(), s.verdict);
    SetTextColor(("mm_ld" + id + "_state").c_str(),
                 Rgba(s.loadable ? 0x5e5e5effu : 0xffffffffu));
  }

  if (mm_save < 0 || mm_save >= static_cast<int>(mm_saves.size()))
    return;
  const GameUi::MenuSave& s = mm_saves[mm_save];
  const bool has_art = s.art != 0;
  if (has_art)
    ugui::SetImageTexture(Need("mm_ld_shot"), s.art, 1.0f, 1.0f);
  SetVisible("mm_ld_shot", has_art);
  SetVisible("mm_ld_noshot", !has_art);
  SetText("mm_ld_micro", s.kind.empty() ? s.slot : (s.slot + "  ·  " + s.kind));
  SetText("mm_ld_name", s.character);
  const base::String world = s.universe < static_cast<int>(mm_universe_names.size())
                                 ? mm_universe_names[s.universe]
                                 : base::String();
  SetText("mm_ld_sub", "Level " + s.level + (world.empty() ? "" : "  ·  " + world));
  SetText("mm_ld_kv0", s.when.empty() ? "-" : s.when);
  SetText("mm_ld_kv1", s.played.empty() ? "-" : s.played);
  SetText("mm_ld_kv2", s.location.empty() ? "-" : s.location);
  SetText("mm_ld_kv3", s.file.empty() ? "-" : s.file);
  SetText("mm_ld_kv4", s.size.empty() ? "-" : s.size);
  // The heading stays a label; the verdict is the row's own state word. With
  // nothing checked yet there is nothing to head, so it goes too.
  SetVisible("mm_ld_order", !s.order.empty());
  SetText("mm_ld_order", "Load order");
  for (int i = 0; i < kMenuDeps; ++i) {
    const base::String id = base::ToString(i);
    const bool shown = i < static_cast<int>(s.order.size());
    SetVisible(("mm_ld_dep" + id).c_str(), shown);
    if (!shown)
      continue;
    const GameUi::MenuRequirement& r = s.order[i];
    SetBackground(("mm_ld_dp" + id).c_str(), Rgba(r.met ? 0xffffffffu : 0x00000000u));
    SetBorderColor(("mm_ld_dp" + id).c_str(), Rgba(r.met ? 0x00000000u : 0xffffff5eu));
    SetStyleField(
        ("mm_ld_dp" + id).c_str(), [](ugui::Style& style, float v) { style.border_width = v; },
        r.met ? 0.0f : 1.0f);
    SetText(("mm_ld_dn" + id).c_str(), r.name);
    SetText(("mm_ld_ds" + id).c_str(), r.state);
    SetTextColor(("mm_ld_ds" + id).c_str(), Rgba(r.met ? 0x5e5e5effu : 0xffffffffu));
  }
  // Resume is the only white thing on the screen, so it has to stop being white
  // the moment it would not work.
  SetBackground("mm_ld_resume", Rgba(s.loadable ? 0xffffffffu : 0x1a1a1affu));
  SetTextColor("mm_ld_resume_t", Rgba(s.loadable ? 0x000000ffu : 0x5e5e5effu));
}

void GameUi::Impl::ApplyMenuJoin() {
  for (int i = 0; i < kMenuJoinTabs; ++i) {
    const base::String id = base::ToString(i);
    const bool on = i == mm_join_tab;
    SetTextColor(("mm_jn_tab" + id + "_t").c_str(), Rgba(on ? 0xffffffffu : 0x5e5e5effu));
    SetVisible(("mm_jn_tab" + id + "_r").c_str(), on);
  }

  const base::Vector<int> rows = ServersForTab();
  const int total = static_cast<int>(rows.size());
  SetVisible("mm_jn_empty", total == 0);
  SetVisible("mm_jn_card", total > 0);
  SetText("mm_jn_count",
          total == 0 ? base::String() : base::ToString(total) + " live");
  SetVisible("mm_jn_statusrow", !mm_server_status.empty());
  SetText("mm_jn_status", mm_server_status);

  bool selected_here = false;
  for (int index : rows)
    if (index == mm_server) {
      selected_here = true;
      break;
    }
  if (!selected_here)
    mm_server = total > 0 ? rows[0] : -1;

  int selected_row = -1;
  for (int i = 0; i < total; ++i)
    if (rows[i] == mm_server) {
      selected_row = i;
      break;
    }
  mm_join_top = base::Clamp(mm_join_top, 0, base::Max(0, total - kMenuListRows));
  if (selected_row >= 0) {
    if (selected_row < mm_join_top)
      mm_join_top = selected_row;
    if (selected_row >= mm_join_top + kMenuListRows)
      mm_join_top = selected_row - kMenuListRows + 1;
  }

  for (int i = 0; i < kMenuListRows; ++i) {
    const int at = mm_join_top + i;
    const bool shown = at < total;
    SetVisible(Pooled("mm_jn", i), shown);
    if (!shown)
      continue;
    const GameUi::MenuServer& s = mm_servers[rows[at]];
    const bool on = rows[at] == mm_server;
    const base::String id = base::ToString(i);
    const u32 body = s.joinable ? (on ? 0xffffffffu : 0xffffffd6u) : 0x5e5e5effu;

    SetBackground(Pooled("mm_jn", i), Rgba(on ? 0xffffff0au : 0x00000000u));
    SetBackground(("mm_jn" + id + "_pip").c_str(),
                  Rgba(s.joinable ? 0xffffffffu : 0x00000000u));
    SetBorderColor(("mm_jn" + id + "_pip").c_str(),
                   Rgba(s.joinable ? 0x00000000u : 0xffffff5eu));
    SetStyleField(
        ("mm_jn" + id + "_pip").c_str(),
        [](ugui::Style& style, float v) { style.border_width = v; }, s.joinable ? 0.0f : 1.0f);
    SetText(("mm_jn" + id + "_name").c_str(), s.name);
    SetTextColor(("mm_jn" + id + "_name").c_str(), Rgba(body));
    SetText(("mm_jn" + id + "_world").c_str(), s.world);
    SetText(("mm_jn" + id + "_kind").c_str(), s.gametype);
    SetTextColor(("mm_jn" + id + "_kind").c_str(), Rgba(on ? 0xffffffffu : 0x5e5e5effu));
    SetText(("mm_jn" + id + "_slots").c_str(), s.players);
    SetText(("mm_jn" + id + "_ping").c_str(), s.ping);
    SetText(("mm_jn" + id + "_state").c_str(), s.entry);
    // A friend hosting, a password, a full server: the states worth acting on
    // read bright, "Open" stays quiet.
    const bool loud = s.entry != "Open" && !s.entry.empty();
    SetTextColor(("mm_jn" + id + "_state").c_str(), Rgba(loud ? 0xffffffffu : 0x5e5e5effu));
  }

  if (mm_server < 0 || mm_server >= static_cast<int>(mm_servers.size()))
    return;
  const GameUi::MenuServer& s = mm_servers[mm_server];
  const bool has_art = s.art != 0;
  if (has_art)
    ugui::SetImageTexture(Need("mm_jn_shot"), s.art, 1.0f, 1.0f);
  SetVisible("mm_jn_shot", has_art);
  SetVisible("mm_jn_noshot", !has_art);
  SetText("mm_jn_micro", s.gametype);
  SetText("mm_jn_name", s.name);
  SetText("mm_jn_sub", s.detail);
  SetText("mm_jn_kv0", s.host.empty() ? s.address : (s.host + "  ·  " + s.address));
  SetText("mm_jn_kv1", s.world.empty() ? "-" : s.world);
  SetText("mm_jn_kv2", s.arrive.empty() ? "-" : s.arrive);
  SetText("mm_jn_kv3", s.progress.empty() ? "-" : s.progress);
  SetText("mm_jn_kv4", s.players + (s.ping.empty() ? "" : "  ·  " + s.ping));
  SetText("mm_jn_order", s.note.empty() ? base::String("Before you can enter") : s.note);
  for (int i = 0; i < kMenuDeps; ++i) {
    const base::String id = base::ToString(i);
    const bool shown = i < static_cast<int>(s.requirements.size());
    SetVisible(("mm_jn_dep" + id).c_str(), shown);
    if (!shown)
      continue;
    const GameUi::MenuRequirement& r = s.requirements[i];
    SetBackground(("mm_jn_dp" + id).c_str(), Rgba(r.met ? 0xffffffffu : 0x00000000u));
    SetBorderColor(("mm_jn_dp" + id).c_str(), Rgba(r.met ? 0x00000000u : 0xffffff5eu));
    SetStyleField(
        ("mm_jn_dp" + id).c_str(), [](ugui::Style& style, float v) { style.border_width = v; },
        r.met ? 0.0f : 1.0f);
    SetText(("mm_jn_dn" + id).c_str(), r.name);
    SetText(("mm_jn_ds" + id).c_str(), r.state);
    SetTextColor(("mm_jn_ds" + id).c_str(), Rgba(r.met ? 0x5e5e5effu : 0xffffffffu));
  }
  SetBackground("mm_jn_join", Rgba(s.joinable ? 0xffffffffu : 0x1a1a1affu));
  SetTextColor("mm_jn_join_t", Rgba(s.joinable ? 0x000000ffu : 0x5e5e5effu));
}

void GameUi::Impl::LaunchFocusedEntry() {
  // The rail is a second focus axis on the same screen: when a card has it,
  // Enter resumes that save rather than booting the world fresh.
  if (mm_rail >= 0) {
    const base::Vector<int> rail = SavesForUniverse(FocusedUniverse());
    if (mm_rail < static_cast<int>(rail.size())) {
      ResumeSave(rail[mm_rail]);
      return;
    }
  }
  if (mm_entry < 0 || mm_entry >= static_cast<int>(mm_entries.size()))
    return;
  const GameUi::MenuEntry& e = mm_entries[mm_entry];
  if (!e.available)
    return;
  // A mode boots the game it is mounted on, with its manifest id armed; a game
  // boots with an empty mode_id and whatever mounts itself.
  mm_request.kind = MainMenuRequest::Kind::kEnterUniverse;
  mm_request.universe = e.universe;
  mm_request.mode_id = e.mode_id;
  // Friends and Public both host; the segment is what the engine reads to know
  // whether the session is listed.
  mm_request.session = mm_session;
  mm_request.multiplayer = mm_session != MenuSession::kSolo;
}


bool GameUi::Impl::RouteMainMenuClick(ugui::wid target) {
  if (!main_menu_open)
    return false;
  ugui::wid w = target;
  for (int depth = 0; depth < 10 && w.valid(); ++depth) {
    const ugui::WidgetNode* n = ui.world().Get<ugui::WidgetNode>(w);
    if (n) {
      const base::String name = n->name.c_str();
      // Match "<prefix><digit>" so only the pooled names hit, never a longer
      // name that happens to start the same way.
      auto pref = [&](const char* p) -> int {
        const size_t pl = std::strlen(p);
        if (name.size() > pl && name.compare(0, pl, p) == 0 && name[pl] >= '0' && name[pl] <= '9')
          return std::atoi(name.c_str() + pl);
        return -1;
      };
      // A tile slot is a position on the page; the entry it holds depends on it.
      const int count = static_cast<int>(mm_entries.size());
      auto focusSlot = [&](int slot) {
        const int index = mm_page() * kMenuTiles + slot;
        if (index < count)
          mm_entry = index;
        mm_rail = -1;  // focus is back in the grid
      };
      using K = MainMenuRequest::Kind;
      if (name == "mm_back") {
        mm_screen = 0;
        return true;
      }
      if (name == "mm_util_tour") {
        // The engine owns which mode and which world the tour is; the menu only
        // says that it was asked for.
        if (mm_tour_available)
          mm_request.kind = K::kEnterTour;
        return true;
      }
      if (name == "mm_util_mods") {
        mm_screen = 2;
        return true;
      }
      if (name == "mm_util_settings") {
        mm_screen = 3;
        return true;
      }
      if (name == "mm_util_profile") {
        mm_screen = 4;
        return true;
      }
      if (name == "mm_util_quit") {
        mm_request.kind = K::kQuit;
        return true;
      }
      if (name == "mm_mp_host") {
        mm_mp_mode = 0;
        mm_request.kind = K::kHostServer;
        mm_request.universe = mm_entry < count ? mm_entries[mm_entry].universe : 0;
        // Without this the request carries the default kSolo and the engine
        // reads "do not announce", so hosting from here was never listed even
        // with PUBLIC selected.
        mm_request.session = mm_session;
        return true;
      }
      if (name == "mm_mp_join") {
        mm_mp_mode = 1;
        mm_request.kind = K::kJoinServer;
        mm_request.universe = mm_entry < count ? mm_entries[mm_entry].universe : 0;
        return true;
      }
      if (name == "mm_util_join") {
        mm_screen = 6;
        return true;
      }
      if (name == "mm_rail_all") {
        // The rail's overflow: everything six cards cannot hold, on the world
        // the rail is already showing.
        const base::Vector<int> tabs = LoadTabUniverses();
        const int focused = FocusedUniverse();
        for (int t = 0; t < static_cast<int>(tabs.size()); ++t)
          if (tabs[t] == focused)
            mm_load_tab = t;
        mm_screen = 5;
        return true;
      }
      if (name == "mm_ld_resume") {
        ResumeSave(mm_save);
        return true;
      }
      if (name == "mm_ld_file") {
        // SDL_OpenURL takes a file:// url and hands it to the desktop's own
        // handler, which is what "show me this save on disk" means.
        if (mm_save >= 0 && mm_save < static_cast<int>(mm_saves.size()) &&
            !mm_saves[mm_save].path.empty()) {
          mm_request.kind = K::kOpenUrl;
          mm_request.url = "file://" + mm_saves[mm_save].path;
        }
        return true;
      }
      if (name == "mm_jn_join") {
        JoinServer(mm_server);
        return true;
      }
      if (name == "mm_jn_char") {
        // The character picker in the small: every save of that world is a
        // character you could bring, so this walks them.
        if (mm_server >= 0 && mm_server < static_cast<int>(mm_servers.size())) {
          const base::Vector<int> mine = SavesForUniverse(mm_servers[mm_server].universe);
          if (!mine.empty()) {
            int at = 0;
            for (int i = 0; i < static_cast<int>(mine.size()); ++i)
              if (mine[i] == mm_save)
                at = i + 1;
            mm_save = mine[at % static_cast<int>(mine.size())];
            mm_servers[mm_server].arrive = mm_saves[mm_save].character + "  ·  level " +
                                           mm_saves[mm_save].level;
          }
        }
        return true;
      }
      if (int i = pref("mm_sess"); i >= 0 && i < 3) {
        mm_session = static_cast<MenuSession>(i);
        return true;
      }
      if (int i = pref("mm_ld_tab"); i >= 0) {
        mm_load_tab = i;
        mm_load_top = 0;
        return true;
      }
      if (int i = pref("mm_jn_tab"); i >= 0) {
        mm_join_tab = i;
        mm_join_top = 0;
        return true;
      }
      // A rail card takes focus off the grid: the two axes cannot both hold it.
      // One click picks the moment, two resume it, which is what every save
      // list in every game does and what the button alone does not give.
      if (int i = pref("mm_sv"); i >= 0) {
        const base::Vector<int> rail = SavesForUniverse(FocusedUniverse());
        if (i < static_cast<int>(rail.size())) {
          mm_rail = i;
          mm_save = rail[i];
          if (DoubleClicked("sv" + base::ToString(mm_save)))
            ResumeSave(mm_save);
        }
        return true;
      }
      if (int i = pref("mm_ld"); i >= 0) {
        const base::Vector<int> rows = SavesForTab();
        const int at = mm_load_top + i;
        if (at < static_cast<int>(rows.size())) {
          mm_save = rows[at];
          if (DoubleClicked("ld" + base::ToString(mm_save)))
            ResumeSave(mm_save);
        }
        return true;
      }
      if (int i = pref("mm_jn"); i >= 0) {
        const base::Vector<int> rows = ServersForTab();
        const int at = mm_join_top + i;
        if (at < static_cast<int>(rows.size())) {
          mm_server = rows[at];
          if (DoubleClicked("jn" + base::ToString(mm_server)))
            JoinServer(mm_server);
        }
        return true;
      }
      // Play sits inside its tile, so it has to win the match on the way up.
      if (int i = pref("mm_play"); i >= 0) {
        focusSlot(i);
        LaunchFocusedEntry();
        return true;
      }
      if (int i = pref("mm_tile"); i >= 0) {
        focusSlot(i);
        if (DoubleClicked("tile" + base::ToString(mm_entry)))
          LaunchFocusedEntry();
        return true;
      }
      if (int i = pref("mm_pip"); i >= 0) {
        if (i * kMenuTiles < count)
          mm_entry = i * kMenuTiles;
        return true;
      }
    }
    const ugui::Hierarchy* h = ui.world().Get<ugui::Hierarchy>(w);
    w = h ? h->parent : ugui::wid{};
  }
  return false;
}

// First-run setup wizard, a parallel overlay to the main menu. It owns its page
// and selections, the engine feeds it the located games and mods dir, and it
// raises a request the engine consumes (browse, launch, cancel).

void GameUi::Impl::ApplyFirstRun() {
  SetVisible("firstrun", first_run_open);
  if (!first_run_open)
    return;
  auto setText = [&](const base::String& n, const base::String& t) {
    SetText(n.c_str(), t.c_str());
  };

  // The front-end palette (theme.ugui): white is "now", dim is "done", dim2 is
  // "not yet". State is carried by value, never by hue.
  constexpr u32 kFg = 0xffffffffu, kDim = 0x9a9a9affu, kDim2 = 0x5e5e5effu;
  constexpr u32 kOff = 0xffffff3du;  // disabled: 24% white

  static const char* const kModes[4] = {"Exploration", "Story", "Survival", "Sandbox"};
  static const char* const kDiffs[4] = {"Novice", "Normal", "Hard", "Legendary"};
  const int mode = base::Clamp(fr_mode, 0, 3), diff = base::Clamp(fr_diff, 0, 3);
  const int located = fr_located(), games = static_cast<int>(fr_view.games.size());
  const base::String mods_dir =
      fr_view.mods_dir.empty() ? base::String("~/.recreation/mods") : fr_view.mods_dir;

  // Pages: exactly one visible.
  for (int i = 0; i < kFirstRunSteps; ++i)
    SetVisible(("fr_step" + base::ToString(i)).c_str(), i == fr_step);

  // A page turn settles rather than snaps: the incoming page fades up and
  // slides the last few pixels into place over kFirstRunPageFade. Opacity
  // inherits in ugui, so setting it on the page carries the whole page.
  if (fr_step != fr_anim_step) {
    fr_anim_step = fr_step;
    fr_anim_from = ui_time;
  }
  const f32 t = base::Clamp((ui_time - fr_anim_from) / kFirstRunPageFade, 0.0f, 1.0f);
  const f32 ease = 1.0f - (1.0f - t) * (1.0f - t);
  const char* const page = Pooled("fr_step", fr_step);
  SetStyleField(
      page, [](ugui::Style& s, float v) { s.opacity = v; }, ease);
  SetStyleField(
      page, [](ugui::Style& s, float v) { s.margin.left = v; }, (1.0f - ease) * 20.0f);

  const base::String step_no = "0" + base::ToString(fr_step + 1);
  setText("fr_bignum", step_no);
  setText("fr_build", mm_stats.build.empty() ? base::String("") : ("v" + mm_stats.build));

  // The step list IS the progress rail: the page you are on takes the marker
  // and the plate, pages behind you stay legible, pages ahead recede.
  for (int i = 0; i < kFirstRunSteps; ++i) {
    const bool active = i == fr_step, done = i < fr_step;
    SetVisible(Pooled("fr_navsel", i), active);
    SetVisible(Pooled("fr_navplate", i), active);
    SetTextColor(Pooled("fr_navnum", i), Rgba(active ? kFg : (done ? kDim : kDim2)));
    SetTextColor(Pooled("fr_navlbl", i), Rgba(active ? kFg : (done ? kDim : kDim2)));
  }
  // Page 1: how many of the three the scan already found.
  setText("fr_w_games",
          base::ToString(located) + " of " + base::ToString(games) + " games found");

  // Page 2: located games. A filled pip is a located install, a hairline one is
  // a slot still waiting for a folder.
  for (int i = 0; i < kFirstRunGames; ++i) {
    const base::String id = base::ToString(i);
    const bool found = i < static_cast<int>(fr_view.games.size()) && fr_view.games[i].located;
    if (i < static_cast<int>(fr_view.games.size()) && !fr_view.games[i].name.empty())
      setText("fr_name" + id, fr_view.games[i].name);
    SetTextColor(Pooled("fr_name", i), Rgba(found ? kFg : kDim2));
    SetBackground(Pooled("fr_pip", i), Rgba(found ? kFg : 0x000000ffu));
    setText("fr_path" + id, found ? fr_view.games[i].path : base::String("No data folder set"));
    SetTextColor(Pooled("fr_path", i), Rgba(found ? kDim : kDim2));
    setText("fr_stat" + id, found ? "Located" : "Not found");
    SetTextColor(Pooled("fr_stat", i), Rgba(found ? kFg : kDim2));
  }
  setText("fr_loccount", base::ToString(located) + " of " + base::ToString(games) + " located");
  // Continue is gated until at least one game is located: solid white when it
  // can be pressed, an inert plate when it cannot.
  SetBackground("fr_next1", Rgba(located > 0 ? 0xe8e8e8ffu : 0x141414ffu));
  SetTextColor("fr_next1_t", Rgba(located > 0 ? 0x000000ffu : kOff));
  // A failed browse outranks the standing hint and is stated at full white, so
  // a folder pick that found nothing cannot look like one that worked.
  if (!fr_view.notice.empty()) {
    setText("fr_lochint", fr_view.notice);
    SetTextColor("fr_lochint", Rgba(kFg));
  } else {
    setText("fr_lochint", located > 0
                              ? base::String("Ready. The rest can be added later from Settings.")
                              : base::String("Locate at least one game to continue."));
    SetTextColor("fr_lochint", Rgba(kDim2));
  }

  // Page 3: two segmented selectors + the toggles. The chosen cell takes the
  // 2px rule and full-white label; the rest sit at 60%.
  for (int k = 0; k < 4; ++k) {
    const bool m = k == mode, d = k == diff;
    SetVisible(Pooled("fr_modebar", k), m);
    SetVisible(Pooled("fr_diffbar", k), d);
    SetTextColor(Pooled("fr_modelbl", k), Rgba(m ? kFg : kDim));
    SetTextColor(Pooled("fr_difflbl", k), Rgba(d ? kFg : kDim));
    SetBackground(Pooled("fr_modeopt", k), Rgba(m ? 0x17191cffu : 0x0d0d0dffu));
    SetBackground(Pooled("fr_diffopt", k), Rgba(d ? 0x17191cffu : 0x0d0d0dffu));
  }
  for (int i = 0; i < kFirstRunChecks; ++i) {
    SetBorderColor(Pooled("fr_chkbox", i), Rgba(fr_check[i] ? kFg : 0xffffff33u));
    SetVisible(Pooled("fr_chkmk", i), fr_check[i]);
  }

  // Page 4: mods dir + the free-space line inside the note.
  setText("fr_modspath_t", mods_dir);
  if (!fr_view.space_label.empty())
    setText("fr_space", "Keep about " + fr_view.space_label + " free for downloaded content.");

  // Page 5: the name field belongs to ugui, which owns the caret and the edits;
  // read it back rather than trying to drive it from here.
  if (ugui::wid name_field = ui.FindWidget("fr_name"); name_field.valid()) {
    if (const ugui::TextInputContent* c = ui.world().Get<ugui::TextInputContent>(name_field))
      fr_username = base::String(c->text.c_str());
  }
  // Page 5: what setup is about to write, in its own words.
  base::String names;
  for (int i = 0; i < games; ++i) {
    if (!fr_view.games[i].located)
      continue;
    if (!names.empty())
      names += "  ·  ";
    names += fr_view.games[i].name;
  }
  setText("fr_sum_games", names.empty() ? base::String("None located") : names);
  SetTextColor("fr_sum_games", Rgba(names.empty() ? kDim2 : kFg));
  setText("fr_sum_mode", base::String(kModes[mode]) + "  ·  " + kDiffs[diff]);
  setText("fr_sum_mods", base::String(fr_check[0] ? "Enabled" : "Disabled") + "  ·  " + mods_dir);
  setText("fr_sum_diag", fr_check[1] ? "Shared anonymously" : "Not shared");
  setText("fr_sum_upd", fr_check[2] ? "Checked on launch" : "Never checked");
  const base::String shown_name =
      fr_username.empty() ? (mm_stats.account.empty() ? base::String("Your account name")
                                                      : mm_stats.account)
                          : fr_username;
  setText("fr_sum_name",
          shown_name + (fr_check[3] ? "  ·  presence on" : "  ·  presence off"));
  SetTextColor("fr_sum_name", Rgba(fr_username.empty() ? kDim : kFg));
}

void GameUi::Impl::AdvanceFirstRun() {
  if (fr_step == 1 && fr_located() == 0)
    return;  // locate page: need one game
  if (fr_step < kFirstRunSteps - 1) {
    ++fr_step;
    return;
  }
  // Last page: advancing launches.
  fr_request.kind = FirstRunRequest::Kind::kLaunch;
  fr_request.mode = fr_mode;
  fr_request.difficulty = fr_diff;
  fr_request.enable_mods = fr_check[0];
  fr_request.share_diagnostics = fr_check[1];
  fr_request.check_updates = fr_check[2];
  fr_request.rich_presence = fr_check[3];
  fr_request.username = fr_username;
}

void GameUi::Impl::RetreatFirstRun() {
  if (fr_step > 0)
    --fr_step;
  else
    fr_request.kind = FirstRunRequest::Kind::kCancel;
}

bool GameUi::Impl::RouteFirstRunClick(ugui::wid target) {
  if (!first_run_open)
    return false;
  ugui::wid w = target;
  for (int depth = 0; depth < 10 && w.valid(); ++depth) {
    const ugui::WidgetNode* n = ui.world().Get<ugui::WidgetNode>(w);
    if (n) {
      const base::String name = n->name.c_str();
      auto pref = [&](const char* p) -> int {
        const size_t pl = std::strlen(p);
        if (name.size() > pl && name.compare(0, pl, p) == 0 && name[pl] >= '0' && name[pl] <= '9')
          return std::atoi(name.c_str() + pl);
        return -1;
      };
      using K = FirstRunRequest::Kind;
      if (name == "fr_begin") {
        AdvanceFirstRun();
        return true;
      }
      if (name == "fr_back1" || name == "fr_back2" || name == "fr_back3" || name == "fr_back4" ||
          name == "fr_back5" || name == "fr_skip") {
        RetreatFirstRun();
        return true;
      }
      // The step list doubles as navigation, but only backwards: a page ahead
      // has not had its gate (a located game) run yet.
      if (int i = pref("fr_nav"); i >= 0 && i < fr_step) {
        fr_step = i;
        return true;
      }
      if (name == "fr_next1" || name == "fr_next2" || name == "fr_next3" || name == "fr_next4") {
        AdvanceFirstRun();
        return true;
      }
      if (name == "fr_launch") {
        AdvanceFirstRun();  // shares the launch path (already on the last page)
        return true;
      }
      if (name == "fr_browse_mods") {
        fr_request.kind = K::kBrowseMods;
        return true;
      }
      if (int i = pref("fr_browse"); i >= 0) {
        fr_request.kind = K::kBrowseGame;
        fr_request.index = i;
        return true;
      }
      if (int k = pref("fr_modeopt"); k >= 0) {
        fr_mode = k;
        return true;
      }
      if (int k = pref("fr_diffopt"); k >= 0) {
        fr_diff = k;
        return true;
      }
      if (int i = pref("fr_chk"); i >= 0 && i < kFirstRunChecks) {
        fr_check[i] = !fr_check[i];
        return true;
      }
    }
    const ugui::Hierarchy* h = ui.world().Get<ugui::Hierarchy>(w);
    w = h ? h->parent : ugui::wid{};
  }
  // The wizard owns every click while it is up: nothing is behind it, and a
  // stray press must not fall through to the menu it is covering.
  return true;
}

// The loading screen. Everything on it comes from the engine's per-phase report
// except the tip, which is this screen's own business: the load is the one
// moment a new player is guaranteed to be reading, so it spends it teaching the
// keys instead of turning a spinner. Every key named here is a real default
// binding (runtime/input/game_input.cc).
void GameUi::Impl::ApplyLoading() {
  SetVisible("loading", loading_open);
  if (!loading_open)
    return;

  struct Tip {
    const char* key;
    const char* text;
  };
  static const Tip kTips[] = {
      {"E", "Activate what you are looking at: talk, open, pick up."},
      {"T", "Swap between walking the world and flying the camera through it."},
      {"M", "Open the world map. Places you have been are marked on it."},
      {"J", "Open the journal: every quest you are carrying, and where it stands."},
      {"C", "Third person. The camera pulls back off your shoulder."},
      {"F4", "Open the map editor and place, move or delete anything, live."},
      {"F3", "Open the quest debugger to watch stages and objectives fire."},
      {"F1", "Show the engine overlay: frame times, streaming, what is resident."},
      {"Esc", "Pause. Settings, controls and the way back to the front screen."},
  };
  constexpr int kTipCount = static_cast<int>(sizeof(kTips) / sizeof(kTips[0]));

  // Which world, is it moving, what is it doing. The records / plugins /
  // elapsed table, the percentage, the build stamp and the step number are all
  // gone with the wizard chassis they belonged to: engineer-facing, and a
  // loading screen has no reader for them. The plugin count survives inside the
  // phase sentence the engine sends ("Reading 5 plugins"), where it is a fact
  // about the work rather than a statistic.
  SetText("ld_title", loading.title);
  SetText("ld_phase", loading.phase);
  SetText("ld_sub", loading.detail);

  // The bar never sits at zero: a load that has only just started still has to
  // read as started, or the screen looks as stuck as the freeze it replaced.
  const int pct = base::Clamp(static_cast<int>(loading.progress * 100.0f), 1, 100);
  SetStyleField(
      "ld_prog", [](ugui::Style& s, float v) { s.width = ugui::Length::Pct(v); },
      static_cast<float>(pct));

  if (ui_time - ld_tip_from >= kLoadingTipSeconds) {
    ld_tip_from = ui_time;
    ld_tip = (ld_tip + 1) % kTipCount;
    SetText("ld_tipk", kTips[ld_tip].key);
    SetText("ld_tip", kTips[ld_tip].text);
  }
}


}  // namespace rx

#endif  // RECREATION_HAS_UGUI
