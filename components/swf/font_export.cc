#include "components/swf/font_export.h"

#include <base/containers/unordered_map.h>
#include <base/memory/move.h>

namespace rx::swf {
namespace {

// TrueType's conventional em. Skyrim's DefineFont3 glyphs are authored against
// 20480 units, so the rescale is exact.
constexpr i32 kUnitsPerEm = 2048;

struct Buffer {
  base::Vector<u8> bytes;

  void U8(u8 v) { bytes.push_back(v); }
  void U16(u16 v) {
    U8(static_cast<u8>(v >> 8));
    U8(static_cast<u8>(v));
  }
  void I16(i16 v) { U16(static_cast<u16>(v)); }
  void U32(u32 v) {
    U16(static_cast<u16>(v >> 16));
    U16(static_cast<u16>(v));
  }
  void Utf16Be(base::StringRef text) {
    // The names are ASCII in every shipped font, so one unit per byte is exact.
    for (mem_size i = 0; i < text.size(); ++i)
      U16(static_cast<u16>(static_cast<u8>(text[i])));
  }
  void Align4() {
    while (bytes.size() % 4 != 0)
      U8(0);
  }
  mem_size size() const { return bytes.size(); }
};

struct Point {
  i16 x = 0;
  i16 y = 0;
  bool on_curve = true;
};

struct GlyphOutline {
  base::Vector<Point> points;
  base::Vector<u16> contour_ends;  // last point index of each contour
  i16 x_min = 0, y_min = 0, x_max = 0, y_max = 0;
};

// SWF coordinates are in the font's own units with y growing downward; TrueType
// wants a 2048 em with y growing up.
i16 Scale(i32 value, u32 em_units) {
  const i64 scaled = static_cast<i64>(value) * kUnitsPerEm / static_cast<i64>(em_units);
  if (scaled > 32767)
    return 32767;
  if (scaled < -32768)
    return -32768;
  return static_cast<i16>(scaled);
}

GlyphOutline Convert(const Glyph& glyph, u32 em_units) {
  GlyphOutline out;
  bool first = true;
  for (const Contour& contour : glyph.contours) {
    if (contour.segments.empty())
      continue;
    const mem_size start = out.points.size();

    Point begin;
    begin.x = Scale(contour.segments[0].from_x, em_units);
    begin.y = Scale(-contour.segments[0].from_y, em_units);
    out.points.push_back(begin);

    for (const Segment& segment : contour.segments) {
      if (segment.curved) {
        Point control;
        control.x = Scale(segment.control_x, em_units);
        control.y = Scale(-segment.control_y, em_units);
        control.on_curve = false;
        out.points.push_back(control);
      }
      Point anchor;
      anchor.x = Scale(segment.to_x, em_units);
      anchor.y = Scale(-segment.to_y, em_units);
      out.points.push_back(anchor);
    }

    // TrueType closes every contour implicitly, so a final point back on the
    // start would draw a zero-length edge.
    if (out.points.size() > start + 1) {
      const Point& last = out.points[out.points.size() - 1];
      const Point& begin_point = out.points[start];
      if (last.on_curve && last.x == begin_point.x && last.y == begin_point.y)
        out.points.pop_back();
    }
    if (out.points.size() <= start)
      continue;
    out.contour_ends.push_back(static_cast<u16>(out.points.size() - 1));

    for (mem_size i = start; i < out.points.size(); ++i) {
      const Point& p = out.points[i];
      if (first) {
        out.x_min = out.x_max = p.x;
        out.y_min = out.y_max = p.y;
        first = false;
        continue;
      }
      out.x_min = p.x < out.x_min ? p.x : out.x_min;
      out.x_max = p.x > out.x_max ? p.x : out.x_max;
      out.y_min = p.y < out.y_min ? p.y : out.y_min;
      out.y_max = p.y > out.y_max ? p.y : out.y_max;
    }
  }
  return out;
}

void WriteGlyph(const GlyphOutline& outline, Buffer& out) {
  if (outline.contour_ends.empty())
    return;  // an empty glyph occupies no bytes; loca marks it by a zero span

  out.I16(static_cast<i16>(outline.contour_ends.size()));
  out.I16(outline.x_min);
  out.I16(outline.y_min);
  out.I16(outline.x_max);
  out.I16(outline.y_max);
  for (u16 end : outline.contour_ends)
    out.U16(end);
  out.U16(0);  // no instructions

  // Flags carry only the on-curve bit; every delta is written as a signed word,
  // which is always legal and keeps the writer free of the short-form cases.
  for (const Point& p : outline.points)
    out.U8(p.on_curve ? 0x01 : 0x00);
  i16 previous = 0;
  for (const Point& p : outline.points) {
    out.I16(static_cast<i16>(p.x - previous));
    previous = p.x;
  }
  previous = 0;
  for (const Point& p : outline.points) {
    out.I16(static_cast<i16>(p.y - previous));
    previous = p.y;
  }
  out.Align4();
}

struct Table {
  char tag[4];
  base::Vector<u8> data;
};

u32 Checksum(const base::Vector<u8>& data) {
  u32 sum = 0;
  for (mem_size i = 0; i < data.size(); i += 4) {
    u32 word = 0;
    for (mem_size k = 0; k < 4; ++k)
      word = (word << 8) | (i + k < data.size() ? data[i + k] : 0);
    sum += word;
  }
  return sum;
}

// cmap format 4, built from contiguous runs of character codes.
base::Vector<u8> BuildCmap(const base::Vector<u16>& codes) {
  struct Segment {
    u16 start;
    u16 end;
    i16 delta;
  };
  base::Vector<Segment> segments;
  // codes[i] is the character glyph i draws; TrueType glyph ids are shifted by
  // one because id 0 is .notdef.
  base::Vector<base::Pair<u16, u16>> pairs;  // code -> glyph id
  for (mem_size i = 0; i < codes.size(); ++i) {
    if (codes[i] == 0)
      continue;
    pairs.push_back(base::MakePair(codes[i], static_cast<u16>(i + 1)));
  }
  for (mem_size i = 1; i < pairs.size(); ++i) {
    for (mem_size j = i; j > 0 && pairs[j].first < pairs[j - 1].first; --j) {
      base::Pair<u16, u16> tmp = pairs[j];
      pairs[j] = pairs[j - 1];
      pairs[j - 1] = tmp;
    }
  }
  for (mem_size i = 0; i < pairs.size(); ++i) {
    if (i > 0 && pairs[i].first == pairs[i - 1].first)
      continue;  // two glyphs claiming one code: the first wins
    const i16 delta = static_cast<i16>(pairs[i].second - pairs[i].first);
    if (!segments.empty() && segments[segments.size() - 1].end + 1 == pairs[i].first &&
        segments[segments.size() - 1].delta == delta) {
      segments[segments.size() - 1].end = pairs[i].first;
      continue;
    }
    segments.push_back(Segment{pairs[i].first, pairs[i].first, delta});
  }
  segments.push_back(Segment{0xffff, 0xffff, 1});  // the mandatory terminator

  Buffer sub;
  const u16 seg_count = static_cast<u16>(segments.size());
  u16 search_range = 2;
  u16 entry_selector = 0;
  while (static_cast<u16>(search_range * 2) <= seg_count * 2) {
    search_range = static_cast<u16>(search_range * 2);
    ++entry_selector;
  }
  sub.U16(4);
  sub.U16(static_cast<u16>(16 + seg_count * 8));
  sub.U16(0);
  sub.U16(static_cast<u16>(seg_count * 2));
  sub.U16(search_range);
  sub.U16(entry_selector);
  sub.U16(static_cast<u16>(seg_count * 2 - search_range));
  for (const Segment& s : segments)
    sub.U16(s.end);
  sub.U16(0);
  for (const Segment& s : segments)
    sub.U16(s.start);
  for (const Segment& s : segments)
    sub.I16(s.delta);
  for (mem_size i = 0; i < segments.size(); ++i)
    sub.U16(0);  // idRangeOffset: deltas alone map every segment

  Buffer out;
  out.U16(0);   // version
  out.U16(1);   // one encoding record
  out.U16(3);   // windows
  out.U16(1);   // unicode BMP
  out.U32(12);  // offset to the subtable
  for (mem_size i = 0; i < sub.bytes.size(); ++i)
    out.U8(sub.bytes[i]);
  return base::move(out.bytes);
}

base::Vector<u8> BuildName(base::StringRef family, bool bold, bool italic) {
  const char* style = bold && italic ? "Bold Italic"
                      : bold         ? "Bold"
                      : italic       ? "Italic"
                                     : "Regular";
  base::String full(family);
  full += ' ';
  full += style;
  base::String postscript;
  for (mem_size i = 0; i < family.size(); ++i) {
    const char c = family[i];
    if (c != ' ')
      postscript.push_back(c);
  }
  postscript += '-';
  for (const char* p = style; *p; ++p)
    if (*p != ' ')
      postscript.push_back(*p);

  struct Entry {
    u16 id;
    base::StringRef text;
  };
  const Entry entries[] = {
      {1, family},
      {2, base::StringRef(style)},
      {3, base::StringRef(full)},
      {4, base::StringRef(full)},
      {5, base::StringRef("Version 1.0")},
      {6, base::StringRef(postscript)},
  };
  const u16 count = static_cast<u16>(sizeof(entries) / sizeof(entries[0]));

  Buffer strings;
  base::Vector<u16> offsets;
  base::Vector<u16> lengths;
  for (const Entry& e : entries) {
    offsets.push_back(static_cast<u16>(strings.size()));
    strings.Utf16Be(e.text);
    lengths.push_back(static_cast<u16>(e.text.size() * 2));
  }

  Buffer out;
  out.U16(0);
  out.U16(count);
  out.U16(static_cast<u16>(6 + count * 12));
  for (u16 i = 0; i < count; ++i) {
    out.U16(3);       // windows
    out.U16(1);       // unicode BMP
    out.U16(0x0409);  // en-us
    out.U16(entries[i].id);
    out.U16(lengths[i]);
    out.U16(offsets[i]);
  }
  for (mem_size i = 0; i < strings.bytes.size(); ++i)
    out.U8(strings.bytes[i]);
  return base::move(out.bytes);
}

}  // namespace

base::String FontFamilyName(const Font& font) {
  base::StringRef source =
      font.full_name.empty() ? base::StringRef(font.name) : base::StringRef(font.full_name);
  base::String out;
  for (mem_size i = 0; i < source.size(); ++i) {
    const char c = source[i];
    if (i == 0 && c == '$')
      continue;
    out.push_back(c);
  }
  return out.empty() ? base::String("SwfFont") : out;
}

base::Vector<u8> ExportTrueType(const Font& font, base::StringRef family) {
  base::Vector<u8> empty;
  if (font.glyphs.empty())
    return empty;

  const u16 glyph_count = static_cast<u16>(font.glyphs.size() + 1);  // + .notdef

  Buffer glyf;
  base::Vector<u32> loca;
  i16 x_min = 0, y_min = 0, x_max = 0, y_max = 0;
  u16 max_points = 0;
  u16 max_contours = 0;
  bool have_bounds = false;

  loca.push_back(0);
  glyf.Align4();
  loca.push_back(static_cast<u32>(glyf.size()));  // .notdef: empty

  for (const Glyph& glyph : font.glyphs) {
    const GlyphOutline outline = Convert(glyph, font.em_units);
    WriteGlyph(outline, glyf);
    loca.push_back(static_cast<u32>(glyf.size()));
    if (outline.points.size() > max_points)
      max_points = static_cast<u16>(outline.points.size());
    if (outline.contour_ends.size() > max_contours)
      max_contours = static_cast<u16>(outline.contour_ends.size());
    if (outline.contour_ends.empty())
      continue;
    if (!have_bounds) {
      x_min = outline.x_min;
      y_min = outline.y_min;
      x_max = outline.x_max;
      y_max = outline.y_max;
      have_bounds = true;
      continue;
    }
    x_min = outline.x_min < x_min ? outline.x_min : x_min;
    y_min = outline.y_min < y_min ? outline.y_min : y_min;
    x_max = outline.x_max > x_max ? outline.x_max : x_max;
    y_max = outline.y_max > y_max ? outline.y_max : y_max;
  }

  // Metrics. A font without a layout block gets the usual 0.8/0.2 split, which
  // is what Flash itself falls back to.
  const i16 ascent =
      font.ascent != 0 ? Scale(font.ascent, font.em_units) : static_cast<i16>(1638);
  const i16 descent =
      font.descent != 0 ? Scale(font.descent, font.em_units) : static_cast<i16>(410);
  const i16 line_gap = Scale(font.leading, font.em_units);

  Buffer hmtx;
  u16 advance_max = 0;
  hmtx.U16(0);  // .notdef
  hmtx.I16(0);
  for (mem_size i = 0; i < font.glyphs.size(); ++i) {
    const i16 advance =
        i < font.advances.size() ? Scale(font.advances[i], font.em_units) : 0;
    const u16 unsigned_advance = advance > 0 ? static_cast<u16>(advance) : 0;
    advance_max = unsigned_advance > advance_max ? unsigned_advance : advance_max;
    hmtx.U16(unsigned_advance);
    hmtx.I16(0);
  }

  Buffer head;
  head.U32(0x00010000);
  head.U32(0x00010000);
  head.U32(0);  // checksum adjustment: left zero, no reader here verifies it
  head.U32(0x5f0f3cf5);
  head.U16(0x0003);
  head.U16(static_cast<u16>(kUnitsPerEm));
  for (int i = 0; i < 4; ++i)
    head.U32(0);  // created / modified
  head.I16(x_min);
  head.I16(y_min);
  head.I16(x_max);
  head.I16(y_max);
  head.U16(static_cast<u16>((font.bold ? 1 : 0) | (font.italic ? 2 : 0)));
  head.U16(8);
  head.I16(2);
  head.I16(1);  // long loca
  head.I16(0);

  Buffer hhea;
  hhea.U32(0x00010000);
  hhea.I16(ascent);
  hhea.I16(static_cast<i16>(-descent));
  hhea.I16(line_gap);
  hhea.U16(advance_max);
  hhea.I16(x_min);
  hhea.I16(0);
  hhea.I16(x_max);
  hhea.I16(1);
  hhea.I16(0);
  hhea.I16(0);
  for (int i = 0; i < 4; ++i)
    hhea.I16(0);
  hhea.I16(0);
  hhea.U16(glyph_count);

  Buffer maxp;
  maxp.U32(0x00010000);
  maxp.U16(glyph_count);
  maxp.U16(max_points);
  maxp.U16(max_contours);
  maxp.U16(0);
  maxp.U16(0);
  maxp.U16(2);
  maxp.U16(0);
  maxp.U16(0);
  maxp.U16(0);
  maxp.U16(0);
  maxp.U16(0);
  maxp.U16(0);
  maxp.U16(0);
  maxp.U16(0);

  Buffer os2;
  os2.U16(4);
  os2.I16(static_cast<i16>(advance_max / 2));
  os2.U16(font.bold ? 700 : 400);
  os2.U16(5);
  os2.U16(0);
  os2.I16(1331);
  os2.I16(1433);
  os2.I16(0);
  os2.I16(287);
  os2.I16(1331);
  os2.I16(1433);
  os2.I16(0);
  os2.I16(983);
  os2.I16(102);
  os2.I16(530);
  os2.I16(0);
  for (int i = 0; i < 10; ++i)
    os2.U8(0);  // panose
  os2.U32(1);
  os2.U32(0);
  os2.U32(0);
  os2.U32(0);
  for (const char c : {'R', 'E', 'C', 'R'})
    os2.U8(static_cast<u8>(c));
  os2.U16(font.italic ? 0x0001 : 0x0040);
  u16 first_char = 0xffff;
  u16 last_char = 0;
  for (u16 code : font.code_table) {
    if (code == 0)
      continue;
    first_char = code < first_char ? code : first_char;
    last_char = code > last_char ? code : last_char;
  }
  os2.U16(first_char == 0xffff ? 0x20 : first_char);
  os2.U16(last_char);
  os2.I16(ascent);
  os2.I16(static_cast<i16>(-descent));
  os2.I16(line_gap);
  os2.U16(static_cast<u16>(ascent));
  os2.U16(static_cast<u16>(descent));
  os2.U32(1);
  os2.U32(0);
  os2.I16(static_cast<i16>(ascent / 2));
  os2.I16(ascent);
  os2.U16(0x20);
  os2.U16(0x20);
  os2.U16(1);

  Buffer post;
  post.U32(0x00030000);
  post.U32(0);
  post.I16(static_cast<i16>(-descent / 2));
  post.I16(102);
  post.U32(0);
  for (int i = 0; i < 4; ++i)
    post.U32(0);

  Buffer loca_table;
  for (u32 offset : loca)
    loca_table.U32(offset);

  base::Vector<Table> tables;
  auto add = [&tables](const char* tag, base::Vector<u8> data) {
    Table table;
    for (int i = 0; i < 4; ++i)
      table.tag[i] = tag[i];
    table.data = base::move(data);
    tables.push_back(base::move(table));
  };
  // Sorted by tag, which is what the directory requires.
  add("OS/2", base::move(os2.bytes));
  add("cmap", BuildCmap(font.code_table));
  add("glyf", base::move(glyf.bytes));
  add("head", base::move(head.bytes));
  add("hhea", base::move(hhea.bytes));
  add("hmtx", base::move(hmtx.bytes));
  add("loca", base::move(loca_table.bytes));
  add("maxp", base::move(maxp.bytes));
  add("name", BuildName(family, font.bold, font.italic));
  add("post", base::move(post.bytes));

  const u16 count = static_cast<u16>(tables.size());
  u16 search_range = 16;
  u16 entry_selector = 0;
  while (static_cast<u32>(search_range) * 2 <= static_cast<u32>(count) * 16) {
    search_range = static_cast<u16>(search_range * 2);
    ++entry_selector;
  }

  Buffer out;
  out.U32(0x00010000);
  out.U16(count);
  out.U16(search_range);
  out.U16(entry_selector);
  out.U16(static_cast<u16>(count * 16 - search_range));

  u32 offset = static_cast<u32>(12 + count * 16);
  for (const Table& table : tables) {
    for (int i = 0; i < 4; ++i)
      out.U8(static_cast<u8>(table.tag[i]));
    out.U32(Checksum(table.data));
    out.U32(offset);
    out.U32(static_cast<u32>(table.data.size()));
    offset += static_cast<u32>((table.data.size() + 3) & ~mem_size(3));
  }
  for (const Table& table : tables) {
    for (mem_size i = 0; i < table.data.size(); ++i)
      out.U8(table.data[i]);
    out.Align4();
  }
  return base::move(out.bytes);
}

}  // namespace rx::swf
