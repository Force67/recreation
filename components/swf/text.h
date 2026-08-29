#ifndef RECREATION_SWF_TEXT_H_
#define RECREATION_SWF_TEXT_H_

#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/swf/shape.h"
#include "components/swf/types.h"
#include "core/types.h"

namespace rx::swf {

enum class TextAlign : u8 { kLeft = 0, kRight = 1, kCenter = 2, kJustify = 3 };

// A DefineEditText character: the dynamic text fields the menus write into from
// ActionScript. `variable` is the AS2 path the field is bound to, which is the
// single most useful thing in the file for rebuilding the UI, since it names
// which piece of game state each label shows.
struct EditText {
  u16 id = 0;
  Rect bounds;  // twips, local to the character
  u16 font_id = 0;
  base::String font_class;
  u16 font_height = 0;  // twips
  Rgba color{0, 0, 0, 255};
  u16 max_length = 0;
  TextAlign align = TextAlign::kLeft;
  u16 left_margin = 0;
  u16 right_margin = 0;
  u16 indent = 0;
  i16 leading = 0;
  base::String variable;
  base::String initial_text;
  bool has_font = false;
  bool has_color = false;
  bool has_layout = false;
  bool word_wrap = false;
  bool multiline = false;
  bool password = false;
  bool read_only = false;
  bool auto_size = false;
  bool no_select = false;
  bool border = false;
  bool html = false;
  bool use_outlines = false;
};

// One run of glyphs inside a DefineText: everything between two style changes.
struct TextRun {
  u16 font_id = 0;
  u16 height = 0;  // twips
  Rgba color{0, 0, 0, 255};
  i16 x_offset = 0;
  i16 y_offset = 0;
  bool has_x_offset = false;
  bool has_y_offset = false;
  base::Vector<u16> glyphs;    // indices into the font's glyph table
  base::Vector<i16> advances;  // twips, one per glyph
};

// A DefineText/DefineText2 character: text baked at export time. The glyph
// indices only become readable once resolved through the font's code table,
// which Movie does when it has both characters.
struct StaticText {
  u16 id = 0;
  Rect bounds;
  Matrix matrix;
  base::Vector<TextRun> runs;
};

// One glyph's outline, in the font's own coordinate space (see Font::em_units).
struct Glyph {
  base::Vector<Contour> contours;
};

struct Font {
  u16 id = 0;
  base::String name;        // as authored, e.g. "$EverywhereMediumFont"
  base::String full_name;   // from DefineFontName, when present
  bool bold = false;
  bool italic = false;
  bool wide_codes = false;
  i16 ascent = 0;
  i16 descent = 0;
  i16 leading = 0;
  // Coordinate units per em. DefineFont2 stores glyphs against an em of 1024;
  // DefineFont3 keeps twenty times the resolution against the same em.
  u32 em_units = 1024;
  // Parallel to the glyph table: code_table[i] is the character glyph i draws.
  base::Vector<u16> code_table;
  base::Vector<i16> advances;
  base::Vector<Glyph> glyphs;
};

bool ParseEditText(ByteSpan body, EditText& out);

// `tag_code` picks DefineText (11, RGB) or DefineText2 (33, RGBA).
bool ParseStaticText(u16 tag_code, ByteSpan body, StaticText& out);

// `tag_code` picks DefineFont2 (48) or DefineFont3 (75). `with_outlines` reads
// the glyph table as well, which only the font exporter needs.
bool ParseFont(u16 tag_code, ByteSpan body, Font& out, bool with_outlines = false);

// DefineFontName (88): the human-readable family name for an already defined
// font. Fills `full_name` on the matching font.
bool ParseFontName(ByteSpan body, u16& font_id, base::String& name);

// Resolves a static text's glyph indices through `font` into UTF-8.
base::String ResolveRunText(const Font& font, const TextRun& run);

// The formatting Scaleform keeps as HTML inside an edit text's value. A menu
// authored this way stores its real face, size, colour, spacing and alignment
// in the markup rather than in the DefineEditText fields, so reading only the
// tag gives the wrong typeface at the wrong size.
struct HtmlFormat {
  base::String face;  // font symbol, e.g. "$EverywhereMediumFont"
  f32 size = 0;       // pixels at the authored stage; 0 when absent
  Rgba color;
  f32 letter_spacing = 0;
  TextAlign align = TextAlign::kLeft;
  bool has_color = false;
  bool has_align = false;
};

// Reads the <p align> and <font face/size/color/letterSpacing> attributes out of
// an edit text's HTML value. Returns false when the value carries no markup.
bool ParseHtmlFormat(base::StringRef html, HtmlFormat& out);

// Strips the HTML markup Scaleform allows in an edit text's initial value,
// leaving the visible characters. Entities (&lt; &gt; &amp; &quot; &apos;
// &nbsp;) are decoded; everything else inside angle brackets is dropped.
base::String StripHtml(base::StringRef html);

}  // namespace rx::swf

#endif  // RECREATION_SWF_TEXT_H_
