#ifndef RECREATION_SWF_MOVIE_H_
#define RECREATION_SWF_MOVIE_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/optional.h>
#include <base/strings/xstring.h>

#include "components/swf/bitmap.h"
#include "components/swf/shape.h"
#include "components/swf/swf.h"
#include "components/swf/text.h"
#include "components/swf/types.h"
#include "core/types.h"

namespace rx::swf {

enum class CharacterKind : u8 {
  kUnknown,
  kShape,
  kBitmap,
  kEditText,
  kStaticText,
  kFont,
  kSprite,
  kButton,
  kExternalImage,
};

// Where a character id lives: which of the Movie's typed arrays, and at what
// index. The dictionary is one map so a display-list walk resolves any id
// without knowing its kind up front.
struct CharacterRef {
  CharacterKind kind = CharacterKind::kUnknown;
  u32 index = 0;
};

// One PlaceObject on a timeline. A place with `move` set and no character id
// re-styles whatever already sits at that depth.
struct Place {
  u16 depth = 0;
  u16 character_id = 0;
  base::String name;  // instance name, the handle ActionScript uses
  Matrix matrix;
  ColorTransform color_transform;
  u16 clip_depth = 0;  // non-zero: this object masks the depths below it
  u16 ratio = 0;
  u8 blend_mode = 0;
  bool move = false;
  bool has_character = false;
  bool has_matrix = false;
  bool has_color_transform = false;
  bool visible = true;
  // Clip event handlers attached to this instance (on(release), onEnterFrame,
  // ...). The span points into the SwfFile's decompressed body.
  base::Vector<u32> clip_event_flags;
  base::Vector<ByteSpan> clip_event_code;
};

struct Frame {
  base::String label;
  base::Vector<Place> places;
  base::Vector<u16> removes;  // depths cleared this frame
};

// A character whose pixels live outside the movie.
//
// The .gfx export replaces every embedded bitmap with one of these: the art
// moves to a texture file beside the movie and the tag keeps only its size and
// the file to load. Skyrim's quest_journal.gfx carries 163 of them where the
// .swf twin carries 163 bitmaps. `file` is relative to the movie, so the host
// resolves it the same way it found the movie itself.
struct ExternalImage {
  u16 id = 0;
  u16 width = 0;
  u16 height = 0;
  base::String name;  // the export name, e.g. "360_Start.png"
  base::String file;  // what to load, e.g. "360_Start.png.dds"
};

// A timeline: the movie root (id 0) or a DefineSprite.
struct Timeline {
  u16 id = 0;
  base::Vector<Frame> frames;
};

struct ButtonRecord {
  u16 character_id = 0;
  u16 depth = 0;
  Matrix matrix;
  ColorTransform color_transform;
  bool up = false;
  bool over = false;
  bool down = false;
  bool hit_test = false;
};

struct Button {
  u16 id = 0;
  bool track_as_menu = false;
  base::Vector<ButtonRecord> records;
  base::Vector<u16> condition_flags;  // parallel to `condition_code`
  base::Vector<ByteSpan> condition_code;
};

// An ActionScript block, still as bytecode. `sprite_id` is set for init actions
// (the `#initclip` a class definition compiles into) and zero otherwise.
struct Script {
  enum class Kind : u8 { kFrame, kInit, kClipEvent, kButtonCondition };
  Kind kind = Kind::kFrame;
  u16 timeline_id = 0;  // 0 = root
  u16 sprite_id = 0;    // init actions: the sprite the class is bound to
  u32 frame = 0;
  ByteSpan code;
};

// A whole decoded movie. Every ByteSpan points into `file.body`, so the SwfFile
// that produced a Movie has to outlive it.
struct Movie {
  Rect frame_size;
  f32 frame_rate = 0;
  u16 frame_count = 0;
  Rgba background{255, 255, 255, 255};
  bool gfx = false;

  base::Vector<Shape> shapes;
  base::Vector<Bitmap> bitmaps;
  base::Vector<ExternalImage> external_images;
  base::Vector<EditText> edit_texts;
  base::Vector<StaticText> static_texts;
  base::Vector<Font> fonts;
  base::Vector<Timeline> sprites;
  base::Vector<Button> buttons;
  Timeline root;

  base::UnorderedMap<u16, CharacterRef> characters;
  base::UnorderedMap<u16, base::String> exports;  // character id -> symbol name
  base::UnorderedMap<u16, Rect> scaling_grids;    // character id -> 9-slice splits
  base::Vector<base::String> imports;             // "url#symbol" of ImportAssets2
  // Local character id -> the "url#symbol" an ImportAssets2 bound to it. A menu
  // leaves placeholders for the movies the game splices in, and names its font
  // the same way; both resolve through here.
  base::UnorderedMap<u16, base::String> imported_symbols;
  base::Vector<Script> scripts;
  // DoABC tag bodies: ActionScript 3 bytecode. Skyrim's menus carry none;
  // Fallout 4 and Starfield put all their logic here. See components/swf/abc.h.
  base::Vector<ByteSpan> abc_blocks;

  const Shape* FindShape(u16 id) const;
  const Bitmap* FindBitmap(u16 id) const;
  const EditText* FindEditText(u16 id) const;
  const StaticText* FindStaticText(u16 id) const;
  const Font* FindFont(u16 id) const;
  const ExternalImage* FindExternalImage(u16 id) const;
  const Timeline* FindSprite(u16 id) const;
  const Button* FindButton(u16 id) const;
  // The export name of a character, empty when it was never exported.
  base::StringRef ExportName(u16 id) const;
};

// The display list a timeline has built up by `frame`: every placement from
// frame 0 onwards, with removes applied and moves merged into what they move,
// sorted by depth. A frame carries only what changed, so this is what "the
// clip as it looks on that frame" means, and both the translation and the
// interpreter have to read it the same way.
base::Vector<Place> DisplayListAt(const Timeline& timeline, u32 frame);

// What a character covers, in twips, within one movie's own dictionary: a
// shape's or text field's authored box, a bitmap's pixel size, a button's up
// state, and for a sprite the union of what its frames place. This is what a
// clip's `_width` and `_height` report, and the menus compute layout from them.
//
// The ugui exporter has its own copy of this that also follows ImportAssets2
// placeholders into other movies and caches sprite unions across a whole
// translation; this is the single-movie form the interpreter needs.
Rect CharacterBounds(const Movie& movie, u16 character_id);

// Decodes every tag in `file` into the dictionary and timelines. Tags the
// menus never use (sound, video, morph shapes) are skipped, not failed on, so
// an unexpected movie still yields everything else. `want_font_outlines` also
// decodes every glyph, which only the font exporter needs.
base::Optional<Movie> LoadMovie(const SwfFile& file, bool want_font_outlines = false);

}  // namespace rx::swf

#endif  // RECREATION_SWF_MOVIE_H_
