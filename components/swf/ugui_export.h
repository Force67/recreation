#ifndef RECREATION_SWF_UGUI_EXPORT_H_
#define RECREATION_SWF_UGUI_EXPORT_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "components/swf/movie.h"

namespace rx::swf {

struct UguiExportOptions {
  // Base name of the screen; becomes the root widget's name and the asset
  // directory. Sanitised to identifier characters.
  base::String name = "screen";
  // Multiplies every emitted coordinate and font size. Bethesda authors its
  // menus for a 1280x720 stage and lets Scaleform scale them up; recreation's
  // ugui design space is 1080p, so 1.5 puts a Skyrim screen at the right size.
  f32 scale = 1.0f;
  // Which frame of each timeline to snapshot. Menus build their opening state
  // on frame 0; later frames are alternate states of the same widgets.
  u32 frame = 0;
  // Sprite instances nested deeper than this are placed as empty panels. The
  // shipped menus reach about 8.
  u32 max_depth = 16;
  // Skip characters smaller than this in pixels: Scaleform leaves hairline
  // spacers and hit-test rectangles all over the display list.
  f32 min_size_px = 0.5f;
};

// One file the exporter produced alongside the markup.
struct ExportedAsset {
  base::String file;      // relative path, e.g. "hudmenu/shape_142.svg"
  base::String widget;    // the ugui widget that binds it
  base::Vector<u8> bytes;
};

struct UguiScreen {
  base::String markup;                 // the .ugui document
  base::String manifest;               // "widget<TAB>file" lines, one per image
  base::String script;                 // decompiled ActionScript for the movie
  base::Vector<ExportedAsset> assets;  // SVG and PNG files the markup references
  u32 widget_count = 0;
  u32 skipped_count = 0;               // display objects the translation dropped
};

// Translates a decoded movie into libultragui markup.
//
// The mapping is structural, not a screenshot: a DefineSprite becomes a nested
// panel, a PlaceObject matrix becomes absolute left/top/width/height, a solid
// rectangle becomes a panel background, a gradient rectangle becomes ugui's
// background/background-end pair, vector art becomes an SVG asset, an imported
// bitmap becomes a PNG asset, and every DefineEditText becomes a text widget
// keeping the ActionScript variable it was bound to as a comment, so the
// rebuilt screen can be wired back to the same game state.
UguiScreen ExportUgui(const Movie& movie, const UguiExportOptions& options);

// The decompiled ActionScript of every script in the movie, with a header per
// block naming which timeline, frame or sprite class it came from.
base::String ExportScript(const Movie& movie);

}  // namespace rx::swf

#endif  // RECREATION_SWF_UGUI_EXPORT_H_
