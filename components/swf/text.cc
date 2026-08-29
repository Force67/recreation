#include "components/swf/text.h"

#include <base/memory/move.h>
#include <base/strings/string_ref.h>

namespace rx::swf {
namespace {

void AppendUtf8(base::String& out, u32 code) {
  if (code < 0x80) {
    out.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out.push_back(static_cast<char>(0xc0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xe0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  }
}

bool Matches(base::StringRef html, mem_size at, const char* entity) {
  mem_size i = 0;
  for (; entity[i] != '\0'; ++i) {
    if (at + i >= html.size() || html[at + i] != entity[i])
      return false;
  }
  return true;
}

}  // namespace

bool ParseEditText(ByteSpan body, EditText& out) {
  Reader r(body);
  out.id = r.U16();
  out.bounds = r.ReadRect();

  const bool has_text = r.Bits(1) != 0;
  out.word_wrap = r.Bits(1) != 0;
  out.multiline = r.Bits(1) != 0;
  out.password = r.Bits(1) != 0;
  out.read_only = r.Bits(1) != 0;
  out.has_color = r.Bits(1) != 0;
  const bool has_max_length = r.Bits(1) != 0;
  out.has_font = r.Bits(1) != 0;
  const bool has_font_class = r.Bits(1) != 0;
  out.auto_size = r.Bits(1) != 0;
  out.has_layout = r.Bits(1) != 0;
  out.no_select = r.Bits(1) != 0;
  out.border = r.Bits(1) != 0;
  r.Bits(1);  // was static
  out.html = r.Bits(1) != 0;
  out.use_outlines = r.Bits(1) != 0;
  r.Align();

  if (out.has_font)
    out.font_id = r.U16();
  if (has_font_class)
    out.font_class = r.Str();
  if (out.has_font || has_font_class)
    out.font_height = r.U16();
  if (out.has_color)
    out.color = r.ReadRgba();
  if (has_max_length)
    out.max_length = r.U16();
  if (out.has_layout) {
    const u8 align = r.U8();
    out.align = align <= 3 ? static_cast<TextAlign>(align) : TextAlign::kLeft;
    out.left_margin = r.U16();
    out.right_margin = r.U16();
    out.indent = r.U16();
    out.leading = r.I16();
  }
  out.variable = r.Str();
  if (has_text)
    out.initial_text = r.Str();
  return r.ok();
}

bool ParseStaticText(u16 tag_code, ByteSpan body, StaticText& out) {
  const bool with_alpha = tag_code == 33;
  Reader r(body);
  out.id = r.U16();
  out.bounds = r.ReadRect();
  out.matrix = r.ReadMatrix();
  const u32 glyph_bits = r.U8();
  const u32 advance_bits = r.U8();
  if (!r.ok())
    return false;

  TextRun current;
  bool have_style = false;
  while (r.ok()) {
    const u8 header = r.U8();
    if (header == 0)
      break;
    if (header & 0x80) {
      // Style change: flush the run built so far, then update the state.
      if (have_style && !current.glyphs.empty())
        out.runs.push_back(base::move(current));
      TextRun fresh;
      if (have_style) {
        fresh.font_id = current.font_id;
        fresh.height = current.height;
        fresh.color = current.color;
        fresh.x_offset = current.x_offset;
        fresh.y_offset = current.y_offset;
      }
      current = base::move(fresh);
      have_style = true;
      const bool has_font = (header & 0x08) != 0;
      const bool has_color = (header & 0x04) != 0;
      const bool has_y = (header & 0x02) != 0;
      const bool has_x = (header & 0x01) != 0;
      if (has_font)
        current.font_id = r.U16();
      if (has_color)
        current.color = with_alpha ? r.ReadRgba() : r.ReadRgb();
      if (has_x) {
        current.x_offset = r.I16();
        current.has_x_offset = true;
      }
      if (has_y) {
        current.y_offset = r.I16();
        current.has_y_offset = true;
      }
      if (has_font)
        current.height = r.U16();
      continue;
    }

    const u32 count = header & 0x7f;
    for (u32 i = 0; i < count; ++i) {
      current.glyphs.push_back(static_cast<u16>(r.Bits(glyph_bits)));
      current.advances.push_back(static_cast<i16>(r.SignedBits(advance_bits)));
    }
    r.Align();
  }
  if (have_style && !current.glyphs.empty())
    out.runs.push_back(base::move(current));
  return r.ok();
}

bool ParseFont(u16 tag_code, ByteSpan body, Font& out, bool with_outlines) {
  const bool font3 = tag_code == 75;
  Reader r(body);
  out.id = r.U16();

  const bool has_layout = r.Bits(1) != 0;
  r.Bits(1);  // shift jis
  r.Bits(1);  // small text
  r.Bits(1);  // ansi
  const bool wide_offsets = r.Bits(1) != 0;
  out.wide_codes = r.Bits(1) != 0;
  out.italic = r.Bits(1) != 0;
  out.bold = r.Bits(1) != 0;
  r.Align();
  r.U8();  // language code

  const u8 name_length = r.U8();
  out.name = r.StrN(name_length);
  // The authored name is sometimes stored null-terminated inside its own length.
  while (!out.name.empty() && out.name[out.name.size() - 1] == '\0')
    out.name.pop_back();

  const u16 glyph_count = r.U16();
  if (!r.ok())
    return false;
  if (glyph_count == 0)
    return true;

  // The offset table is relative to its own start; each entry locates one glyph
  // and the last locates the code table, so the outlines can be read (or jumped
  // over) without decoding anything in between.
  const mem_size table_start = r.pos();
  const mem_size offset_size = wide_offsets ? 4u : 2u;
  base::Vector<u32> offsets;
  for (u16 i = 0; i < glyph_count && r.ok(); ++i)
    offsets.push_back(wide_offsets ? r.U32() : r.U16());
  const u32 code_table_offset = wide_offsets ? r.U32() : r.U16();
  if (!r.ok())
    return false;

  out.em_units = font3 ? 20480u : 1024u;
  if (with_outlines) {
    for (u16 i = 0; i < glyph_count; ++i) {
      Glyph glyph;
      r.Seek(table_start + offsets[i]);
      if (!r.ok())
        break;
      // A glyph that fails to decode stays empty rather than dropping the rest
      // of the font; the reader is reseated from the offset table either way.
      ParseGlyphOutline(r, glyph.contours);
      out.glyphs.push_back(base::move(glyph));
    }
  }

  r.Seek(table_start + code_table_offset);
  for (u16 i = 0; i < glyph_count && r.ok(); ++i)
    out.code_table.push_back(out.wide_codes ? r.U16() : r.U8());

  if (has_layout && r.ok()) {
    out.ascent = r.I16();
    out.descent = r.I16();
    out.leading = r.I16();
    for (u16 i = 0; i < glyph_count && r.ok(); ++i)
      out.advances.push_back(r.I16());
  }
  // font3 differs only in that its shape coordinates are in EM/1024 units,
  // which never reach here because the outlines are skipped.
  (void)font3;
  return r.ok();
}

bool ParseFontName(ByteSpan body, u16& font_id, base::String& name) {
  Reader r(body);
  font_id = r.U16();
  name = r.Str();
  return r.ok();
}

base::String ResolveRunText(const Font& font, const TextRun& run) {
  base::String out;
  for (u16 glyph : run.glyphs) {
    if (glyph < font.code_table.size())
      AppendUtf8(out, font.code_table[glyph]);
    else
      out.push_back('?');
  }
  return out;
}

namespace {

// Returns the value of `name="..."` inside one tag's attribute run, or an empty
// view when the attribute is absent.
base::StringRef Attribute(base::StringRef tag, const char* name) {
  const mem_size length = base::StringRef(name).size();
  for (mem_size i = 0; i + length + 2 < tag.size(); ++i) {
    if (i != 0 && tag[i - 1] != ' ')
      continue;
    bool match = true;
    for (mem_size k = 0; k < length && match; ++k)
      match = tag[i + k] == name[k];
    if (!match || tag[i + length] != '=' || tag[i + length + 1] != '"')
      continue;
    const mem_size start = i + length + 2;
    mem_size end = start;
    while (end < tag.size() && tag[end] != '"')
      ++end;
    return tag.substr(start, end - start);
  }
  return base::StringRef();
}

f32 ParseFloat(base::StringRef text) {
  f32 value = 0;
  f32 fraction = 0;
  f32 scale = 0.1f;
  bool negative = false;
  bool after_point = false;
  for (mem_size i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (i == 0 && c == '-') {
      negative = true;
    } else if (c == '.') {
      after_point = true;
    } else if (c >= '0' && c <= '9') {
      if (after_point) {
        fraction += static_cast<f32>(c - '0') * scale;
        scale *= 0.1f;
      } else {
        value = value * 10 + static_cast<f32>(c - '0');
      }
    } else {
      break;
    }
  }
  const f32 out = value + fraction;
  return negative ? -out : out;
}

u8 HexPair(base::StringRef text, mem_size at) {
  auto digit = [](char c) -> u8 {
    if (c >= '0' && c <= '9')
      return static_cast<u8>(c - '0');
    if (c >= 'a' && c <= 'f')
      return static_cast<u8>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
      return static_cast<u8>(c - 'A' + 10);
    return 0;
  };
  if (at + 1 >= text.size())
    return 0;
  return static_cast<u8>(digit(text[at]) * 16 + digit(text[at + 1]));
}

}  // namespace

bool ParseHtmlFormat(base::StringRef html, HtmlFormat& out) {
  bool found = false;
  for (mem_size i = 0; i < html.size(); ++i) {
    if (html[i] != '<')
      continue;
    mem_size end = i + 1;
    while (end < html.size() && html[end] != '>')
      ++end;
    const base::StringRef tag = html.substr(i + 1, end - i - 1);
    i = end;

    if (tag.size() >= 2 && tag[0] == 'p' && (tag[1] == ' ' || tag[1] == '>')) {
      const base::StringRef align = Attribute(tag, "align");
      if (!align.empty()) {
        out.align = align == "right"    ? TextAlign::kRight
                    : align == "center" ? TextAlign::kCenter
                    : align == "justify" ? TextAlign::kJustify
                                        : TextAlign::kLeft;
        out.has_align = true;
        found = true;
      }
      continue;
    }
    if (tag.size() < 4 || tag[0] != 'f' || tag[1] != 'o' || tag[2] != 'n' || tag[3] != 't')
      continue;

    // The first <font> wins: a field styled per run is rare, and ugui gives a
    // text widget one face and size.
    const base::StringRef face = Attribute(tag, "face");
    if (!face.empty() && out.face.empty())
      out.face = base::String(face);
    const base::StringRef size = Attribute(tag, "size");
    if (!size.empty() && out.size == 0)
      out.size = ParseFloat(size);
    const base::StringRef spacing = Attribute(tag, "letterSpacing");
    if (!spacing.empty() && out.letter_spacing == 0)
      out.letter_spacing = ParseFloat(spacing);
    const base::StringRef color = Attribute(tag, "color");
    if (color.size() >= 7 && color[0] == '#' && !out.has_color) {
      out.color.r = HexPair(color, 1);
      out.color.g = HexPair(color, 3);
      out.color.b = HexPair(color, 5);
      out.color.a = 255;
      out.has_color = true;
    }
    found = true;
  }
  return found;
}

base::String StripHtml(base::StringRef html) {
  base::String out;
  bool in_tag = false;
  for (mem_size i = 0; i < html.size(); ++i) {
    const char c = html[i];
    if (in_tag) {
      if (c == '>')
        in_tag = false;
      continue;
    }
    if (c == '<') {
      in_tag = true;
      continue;
    }
    if (c == '&') {
      if (Matches(html, i, "&lt;")) {
        out.push_back('<');
        i += 3;
        continue;
      }
      if (Matches(html, i, "&gt;")) {
        out.push_back('>');
        i += 3;
        continue;
      }
      if (Matches(html, i, "&amp;")) {
        out.push_back('&');
        i += 4;
        continue;
      }
      if (Matches(html, i, "&quot;")) {
        out.push_back('"');
        i += 5;
        continue;
      }
      if (Matches(html, i, "&apos;")) {
        out.push_back('\'');
        i += 5;
        continue;
      }
      if (Matches(html, i, "&nbsp;")) {
        out.push_back(' ');
        i += 5;
        continue;
      }
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace rx::swf
