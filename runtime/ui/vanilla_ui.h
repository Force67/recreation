#ifndef RECREATION_RUNTIME_UI_VANILLA_UI_H_
#define RECREATION_RUNTIME_UI_VANILLA_UI_H_

#include <base/containers/pair.h>
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
  // widget name -> asset path, relative to the screen directory.
  base::Vector<base::Pair<base::String, base::String>> images;
};

// Which screens to load, from RX_VANILLA_UI (comma separated, e.g.
// "hudmenu,startmenu"). Empty when the feature is off.
base::Vector<base::String> VanillaScreenNames();

// Where the translated screens live: RX_VANILLA_UI_DIR, else a build-time
// default beside the other .ugui fragments.
base::String VanillaScreenDir();

// Reads <dir>/<name>.ugui and <dir>/<name>.manifest. Returns false with a
// warning when either is missing.
bool LoadVanillaScreen(base::StringRef dir, base::StringRef name, VanillaScreen& out);

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
