#ifndef RECREATION_RUNTIME_UI_VANILLA_UI_H_
#define RECREATION_RUNTIME_UI_VANILLA_UI_H_

#include <base/containers/pair.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace ugui {
class UIContext;
class TextureBackend;
}  // namespace ugui

namespace rx::ui {

// A screen that came out of a vanilla Scaleform movie: the markup tools/swfdump
// translated, plus the image assets it references.
//
// The translation is offline (see tools/swfdump --ugui). What is left at
// runtime is loading the markup like any other fragment and binding the SVG and
// PNG files back onto the image widgets the manifest names, which is the step
// that makes the original interface show up on libultragui's own renderer.
struct VanillaScreen {
  base::String name;
  base::String markup;
  // The stage the movie was authored against (1280x720 for every Skyrim menu).
  // Scaleform scales that to the viewport, so the host does the same rather
  // than pinning the screen to a corner at its authored size.
  f32 stage_width = 0;
  f32 stage_height = 0;
  // widget name -> asset path, relative to the screen directory.
  base::Vector<base::Pair<base::String, base::String>> images;
  // The font families the markup asks for, so only those are loaded.
  base::Vector<base::String> fonts;
};

// The interface's "$KEY" table written beside the screens (strings.txt), so a
// host driving a menu the way the game does resolves the same strings. Empty
// when the file is absent.
base::UnorderedMap<base::String, base::String> LoadVanillaStrings(base::StringRef dir);

// Small pokes at a translated screen, for a host driving one the way the game
// drives the original. The movies leave placeholder labels behind and stack
// every state's panels into the one frame a static translation can capture, so
// the host writes the real text and puts the states it is not showing away.
//
// `widget` is the instance name the movie gave it. SetVanillaText writes the
// first text anywhere below that widget, since a tab or a row wraps its label.
void SetVanillaText(ugui::UIContext& ui, base::StringRef widget, base::StringRef text);
// The same, restricted to `root`'s subtree. A widget name is the movie's own
// instance name, and two screens loaded together carry the same ones (every
// menu has a VersionText), so an unscoped lookup finds whichever screen came
// first in the document rather than the one meant.
void SetVanillaTextIn(ugui::UIContext& ui,
                      base::StringRef root,
                      base::StringRef widget,
                      base::StringRef text);
void SetVanillaTextColor(ugui::UIContext& ui, base::StringRef widget, u32 rgb);

// Turns a whole subtree on or off. ugui ignores `visibility` and its opacity
// does not inherit, so both directions have to reach every leaf: a panel set to
// zero still draws its children, and a subtree the movie faded out carries the
// alpha on its leaves. Showing therefore overrides whatever the movie's colour
// transform baked in, which is right for a subtree the game itself turns on
// (a `gotoAndStop` onto a visible frame) and wrong for anything else.
void ShowVanillaSubtree(ugui::UIContext& ui, base::StringRef widget, bool show);

// Which screens to load, from RX_VANILLA_UI (comma separated, e.g.
// "hudmenu,startmenu"). Empty when the feature is off.
base::Vector<base::String> VanillaScreenNames();

// Where the translated screens live: RX_VANILLA_UI_DIR, else a build-time
// default beside the other .ugui fragments.
base::String VanillaScreenDir();

// Reads <dir>/<name>.ugui and <dir>/<name>.manifest. Returns false with a
// warning when either is missing.
bool LoadVanillaScreen(base::StringRef dir, base::StringRef name, VanillaScreen& out);

// Frees every texture the last BindVanillaImages uploaded. ugui does not own
// user textures and rebuilding the widget tree does not touch them, so the
// screens have to be released before they are bound again or each hot reload
// orphans a full set of images (a translated screen brings hundreds).
void ReleaseVanillaImages(ugui::TextureBackend& backend);

// Registers the TrueType files in <dir>/fonts that `screens` ask for, so a
// screen's `font:` property resolves to the typeface the game embeds in its own
// font movies. Only the referenced families are loaded: the shipped set spans
// every language, and a glyph atlas holding all of them does not fit. Must run
// before the widget tree is built. Returns how many were registered.
u32 LoadVanillaFonts(ugui::UIContext& ui,
                     base::StringRef dir,
                     const base::Vector<VanillaScreen>& screens);

// Rasterizes every asset the screen's manifest lists and binds it to its widget.
// SVG goes through libultragui's own rasterizer, so the vector art stays vector
// art; PNG is decoded and uploaded as is. Call after the tree has been built.
// Returns the number of widgets bound.
u32 BindVanillaImages(ugui::UIContext& ui,
                      ugui::TextureBackend& backend,
                      base::StringRef dir,
                      const VanillaScreen& screen);

}  // namespace rx::ui

#endif  // RECREATION_RUNTIME_UI_VANILLA_UI_H_
