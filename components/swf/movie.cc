#include "components/swf/movie.h"

#include <base/memory/move.h>

namespace rx::swf {
namespace {

// Advances past a PlaceObject3 filter list. The filters themselves change
// nothing about layout, but their length is variable and the tail of the tag
// (blend mode, visibility, clip actions) sits behind them.
void SkipFilterList(Reader& r) {
  const u8 count = r.U8();
  for (u8 i = 0; i < count && r.ok(); ++i) {
    switch (r.U8()) {
      case 0:
        r.Skip(23);
        break;  // drop shadow
      case 1:
        r.Skip(9);
        break;  // blur
      case 2:
        r.Skip(15);
        break;  // glow
      case 3:
        r.Skip(27);
        break;  // bevel
      case 4:
      case 7: {  // gradient glow / gradient bevel
        const u8 colors = r.U8();
        r.Skip(static_cast<mem_size>(colors) * 5);  // RGBA + ratio per stop
        r.Skip(19);
        break;
      }
      case 5: {  // convolution
        const u8 mx = r.U8();
        const u8 my = r.U8();
        r.Skip(8);  // divisor, bias
        r.Skip(static_cast<mem_size>(mx) * my * 4);
        r.Skip(5);  // default colour, flags
        break;
      }
      case 6:
        r.Skip(80);
        break;  // colour matrix
      default:
        return;  // unknown filter: the rest of the tag is unreadable
    }
  }
}

// CLIPACTIONS trailer of PlaceObject2/3: the on(...) handlers authored on an
// instance. `swf_version` decides the width of the event flag fields.
void ReadClipActions(Reader& r, u8 swf_version, Place& place) {
  r.U16();  // reserved
  if (swf_version >= 6)
    r.U32();  // all event flags
  else
    r.U16();

  while (r.ok()) {
    const u32 flags = swf_version >= 6 ? r.U32() : r.U16();
    if (flags == 0)
      break;
    const u32 size = r.U32();
    if (!r.ok() || size > r.remaining())
      break;
    const mem_size record_end = r.pos() + size;
    if (flags & 0x00020000u)  // key press: a key code precedes the actions
      r.U8();
    const mem_size code_size = record_end > r.pos() ? record_end - r.pos() : 0;
    const ByteSpan code = r.Bytes(code_size);
    if (!r.ok())
      break;
    place.clip_event_flags.push_back(flags);
    place.clip_event_code.push_back(code);
  }
}

bool ReadPlace(u16 tag_code, ByteSpan body, u8 swf_version, Place& out) {
  Reader r(body);
  if (tag_code == 4) {
    // PlaceObject: character and matrix are mandatory, no name, no depth flags.
    out.character_id = r.U16();
    out.has_character = true;
    out.depth = r.U16();
    out.matrix = r.ReadMatrix();
    out.has_matrix = true;
    if (!r.eof()) {
      out.color_transform = r.ReadColorTransform(false);
      out.has_color_transform = true;
    }
    return r.ok();
  }

  const u8 flags = r.U8();
  u8 flags2 = 0;
  if (tag_code == 70)
    flags2 = r.U8();

  out.move = (flags & 0x01) != 0;
  out.depth = r.U16();

  if (tag_code == 70 && ((flags2 & 0x08) || ((flags2 & 0x10) && (flags & 0x02))))
    r.Str();  // class name

  if (flags & 0x02) {
    out.character_id = r.U16();
    out.has_character = true;
  }
  if (flags & 0x04) {
    out.matrix = r.ReadMatrix();
    out.has_matrix = true;
  }
  if (flags & 0x08) {
    out.color_transform = r.ReadColorTransform(true);
    out.has_color_transform = true;
  }
  if (flags & 0x10)
    out.ratio = r.U16();
  if (flags & 0x20)
    out.name = r.Str();
  if (flags & 0x40)
    out.clip_depth = r.U16();

  if (tag_code == 70) {
    if (flags2 & 0x01)
      SkipFilterList(r);
    if (flags2 & 0x02)
      out.blend_mode = r.U8();
    if (flags2 & 0x04)
      r.U8();  // bitmap cache
    if (flags2 & 0x20) {
      out.visible = r.U8() != 0;
      r.ReadRgba();  // background colour
    }
  }
  if (flags & 0x80)
    ReadClipActions(r, swf_version, out);
  return r.ok();
}

bool ReadButton(ByteSpan body, Button& out) {
  Reader r(body);
  out.id = r.U16();
  r.Bits(7);  // reserved
  out.track_as_menu = r.Bits(1) != 0;
  r.Align();
  const u16 action_offset = r.U16();

  while (r.ok()) {
    const u8 flags = r.U8();
    if (flags == 0)
      break;
    ButtonRecord rec;
    rec.hit_test = (flags & 0x08) != 0;
    rec.down = (flags & 0x04) != 0;
    rec.over = (flags & 0x02) != 0;
    rec.up = (flags & 0x01) != 0;
    const bool has_filters = (flags & 0x10) != 0;
    const bool has_blend = (flags & 0x20) != 0;
    rec.character_id = r.U16();
    rec.depth = r.U16();
    rec.matrix = r.ReadMatrix();
    rec.color_transform = r.ReadColorTransform(true);
    if (has_filters)
      SkipFilterList(r);
    if (has_blend)
      r.U8();
    out.records.push_back(base::move(rec));
  }

  // action_offset is measured from the start of that field, i.e. 6 bytes into
  // the tag. Zero means the button carries no condition actions at all.
  if (action_offset == 0 || !r.ok())
    return r.ok();
  r.Seek(6u + action_offset - 2u);
  while (r.ok() && !r.eof()) {
    const u16 size = r.U16();
    const u16 conditions = r.U16();
    if (!r.ok())
      break;
    // size counts from the start of this record; zero marks the last one.
    const mem_size available =
        size == 0 ? r.remaining() : (size >= 4 ? size - 4u : 0u);
    const ByteSpan code = r.Bytes(available > r.remaining() ? r.remaining() : available);
    if (!r.ok())
      break;
    out.condition_flags.push_back(conditions);
    out.condition_code.push_back(code);
    if (size == 0)
      break;
  }
  return true;
}

// The root and every DefineSprite body are the same control-tag stream, so one
// walker builds either: the root from the tags OpenSwf already split out, a
// sprite from the raw bytes nested inside its own tag.
struct TimelineBuilder {
  Movie& movie;
  u8 swf_version;

  void Run(u16 timeline_id, const base::Vector<Tag>& tags, Timeline& out);
  void RunNested(u16 timeline_id, ByteSpan tags_body, Timeline& out);
};

}  // namespace

const Shape* Movie::FindShape(u16 id) const {
  const CharacterRef* ref = characters.find(id);
  if (!ref || ref->kind != CharacterKind::kShape)
    return nullptr;
  return &shapes[ref->index];
}

const Bitmap* Movie::FindBitmap(u16 id) const {
  const CharacterRef* ref = characters.find(id);
  if (!ref || ref->kind != CharacterKind::kBitmap)
    return nullptr;
  return &bitmaps[ref->index];
}

const EditText* Movie::FindEditText(u16 id) const {
  const CharacterRef* ref = characters.find(id);
  if (!ref || ref->kind != CharacterKind::kEditText)
    return nullptr;
  return &edit_texts[ref->index];
}

const StaticText* Movie::FindStaticText(u16 id) const {
  const CharacterRef* ref = characters.find(id);
  if (!ref || ref->kind != CharacterKind::kStaticText)
    return nullptr;
  return &static_texts[ref->index];
}

const Font* Movie::FindFont(u16 id) const {
  const CharacterRef* ref = characters.find(id);
  if (!ref || ref->kind != CharacterKind::kFont)
    return nullptr;
  return &fonts[ref->index];
}

const Timeline* Movie::FindSprite(u16 id) const {
  const CharacterRef* ref = characters.find(id);
  if (!ref || ref->kind != CharacterKind::kSprite)
    return nullptr;
  return &sprites[ref->index];
}

const Button* Movie::FindButton(u16 id) const {
  const CharacterRef* ref = characters.find(id);
  if (!ref || ref->kind != CharacterKind::kButton)
    return nullptr;
  return &buttons[ref->index];
}

base::StringRef Movie::ExportName(u16 id) const {
  const base::String* name = exports.find(id);
  return name ? base::StringRef(*name) : base::StringRef();
}

base::Optional<Movie> LoadMovie(const SwfFile& file) {
  Movie movie;
  movie.frame_size = file.frame_size;
  movie.frame_rate = file.frame_rate;
  movie.frame_count = file.frame_count;
  movie.gfx = file.gfx;

  ByteSpan jpeg_tables;
  for (const Tag& tag : file.tags) {
    if (tag.code == static_cast<u16>(TagCode::kJpegTables)) {
      jpeg_tables = tag.body;
      break;
    }
  }

  // Definition tags first: a sprite's display list can name a character defined
  // after it, and the exporter wants a complete dictionary either way.
  base::Vector<const Tag*> sprite_tags;
  for (const Tag& tag : file.tags) {
    switch (static_cast<TagCode>(tag.code)) {
      case TagCode::kDefineShape:
      case TagCode::kDefineShape2:
      case TagCode::kDefineShape3:
      case TagCode::kDefineShape4: {
        Shape shape;
        if (!ParseShape(tag.code, tag.body, shape))
          break;
        movie.characters[shape.id] =
            CharacterRef{CharacterKind::kShape, static_cast<u32>(movie.shapes.size())};
        movie.shapes.push_back(base::move(shape));
        break;
      }
      case TagCode::kDefineBits:
      case TagCode::kDefineBitsJpeg2:
      case TagCode::kDefineBitsJpeg3:
      case TagCode::kDefineBitsLossless:
      case TagCode::kDefineBitsLossless2: {
        Bitmap bitmap;
        if (!ParseBitmap(tag.code, tag.body, jpeg_tables, bitmap))
          break;
        movie.characters[bitmap.id] =
            CharacterRef{CharacterKind::kBitmap, static_cast<u32>(movie.bitmaps.size())};
        movie.bitmaps.push_back(base::move(bitmap));
        break;
      }
      case TagCode::kDefineEditText: {
        EditText text;
        if (!ParseEditText(tag.body, text))
          break;
        movie.characters[text.id] = CharacterRef{CharacterKind::kEditText,
                                                 static_cast<u32>(movie.edit_texts.size())};
        movie.edit_texts.push_back(base::move(text));
        break;
      }
      case TagCode::kDefineText:
      case TagCode::kDefineText2: {
        StaticText text;
        if (!ParseStaticText(tag.code, tag.body, text))
          break;
        movie.characters[text.id] = CharacterRef{
            CharacterKind::kStaticText, static_cast<u32>(movie.static_texts.size())};
        movie.static_texts.push_back(base::move(text));
        break;
      }
      case TagCode::kDefineFont2:
      case TagCode::kDefineFont3: {
        Font font;
        if (!ParseFont(tag.code, tag.body, font))
          break;
        movie.characters[font.id] =
            CharacterRef{CharacterKind::kFont, static_cast<u32>(movie.fonts.size())};
        movie.fonts.push_back(base::move(font));
        break;
      }
      case TagCode::kDefineFontName: {
        u16 font_id = 0;
        base::String name;
        if (!ParseFontName(tag.body, font_id, name))
          break;
        const CharacterRef* ref = movie.characters.find(font_id);
        if (ref && ref->kind == CharacterKind::kFont)
          movie.fonts[ref->index].full_name = base::move(name);
        break;
      }
      case TagCode::kDefineButton2: {
        Button button;
        if (!ReadButton(tag.body, button))
          break;
        movie.characters[button.id] =
            CharacterRef{CharacterKind::kButton, static_cast<u32>(movie.buttons.size())};
        movie.buttons.push_back(base::move(button));
        break;
      }
      case TagCode::kDefineSprite: {
        Reader r(tag.body);
        const u16 id = r.U16();
        r.U16();  // frame count, recovered from the ShowFrames below
        if (!r.ok())
          break;
        movie.characters[id] =
            CharacterRef{CharacterKind::kSprite, static_cast<u32>(movie.sprites.size())};
        Timeline timeline;
        timeline.id = id;
        movie.sprites.push_back(base::move(timeline));
        sprite_tags.push_back(&tag);
        break;
      }
      case TagCode::kDoAbc:
        movie.abc_blocks.push_back(tag.body);
        break;
      case TagCode::kSymbolClass: {
        // The ActionScript 3 equivalent of ExportAssets: it binds a character
        // to the class that drives it, which is the same naming the exporter
        // wants for a widget.
        Reader r(tag.body);
        const u16 count = r.U16();
        for (u16 i = 0; i < count && r.ok(); ++i) {
          const u16 id = r.U16();
          base::String name = r.Str();
          if (id != 0)
            movie.exports[id] = base::move(name);
        }
        break;
      }
      case TagCode::kExportAssets: {
        Reader r(tag.body);
        const u16 count = r.U16();
        for (u16 i = 0; i < count && r.ok(); ++i) {
          const u16 id = r.U16();
          movie.exports[id] = r.Str();
        }
        break;
      }
      case TagCode::kImportAssets2:
      case TagCode::kImportAssets: {
        Reader r(tag.body);
        const base::String url = r.Str();
        if (tag.code == static_cast<u16>(TagCode::kImportAssets2)) {
          r.U8();  // reserved (1)
          r.U8();  // reserved (0)
        }
        const u16 count = r.U16();
        for (u16 i = 0; i < count && r.ok(); ++i) {
          r.U16();  // id assigned locally, unused by the exporter
          base::String entry = url;
          entry += '#';
          entry += r.Str();
          movie.imports.push_back(base::move(entry));
        }
        break;
      }
      case TagCode::kDefineScalingGrid: {
        Reader r(tag.body);
        const u16 id = r.U16();
        const Rect splitter = r.ReadRect();
        if (r.ok())
          movie.scaling_grids[id] = splitter;
        break;
      }
      case TagCode::kSetBackgroundColor: {
        Reader r(tag.body);
        movie.background = r.ReadRgb();
        break;
      }
      default:
        break;
    }
  }

  TimelineBuilder builder{movie, file.version};

  // Sprite display lists, in definition order, then the root.
  for (mem_size i = 0; i < sprite_tags.size(); ++i) {
    const Tag& tag = *sprite_tags[i];
    Reader r(tag.body);
    const u16 id = r.U16();
    r.U16();
    const CharacterRef* ref = movie.characters.find(id);
    if (!ref || ref->kind != CharacterKind::kSprite)
      continue;
    // The sprite's nested tag stream starts right after its two header words.
    builder.RunNested(id, tag.body.subspan(4), movie.sprites[ref->index]);
  }

  builder.Run(0, file.tags, movie.root);
  return base::Optional<Movie>(base::move(movie));
}

namespace {

void TimelineBuilder::RunNested(u16 timeline_id, ByteSpan tags_body, Timeline& out) {
  base::Vector<Tag> tags;
  Reader r(tags_body);
  while (r.ok() && !r.eof()) {
    const u16 header = r.U16();
    const u16 code = static_cast<u16>(header >> 6);
    u32 length = header & 0x3fu;
    if (length == 0x3f)
      length = r.U32();
    if (!r.ok())
      break;
    const u32 offset = static_cast<u32>(r.pos());
    const ByteSpan body = r.Bytes(length);
    if (!r.ok())
      break;
    tags.push_back(Tag{code, body, offset});
    if (code == static_cast<u16>(TagCode::kEnd))
      break;
  }
  Run(timeline_id, tags, out);
}

void TimelineBuilder::Run(u16 timeline_id, const base::Vector<Tag>& tags, Timeline& out) {
  Frame frame;
  u32 frame_index = 0;

  for (const Tag& tag : tags) {
    const u16 code = tag.code;
    const ByteSpan body = tag.body;
    switch (static_cast<TagCode>(code)) {
      case TagCode::kEnd:
        break;
      case TagCode::kShowFrame:
        out.frames.push_back(base::move(frame));
        frame = Frame{};
        ++frame_index;
        break;
      case TagCode::kPlaceObject:
      case TagCode::kPlaceObject2:
      case TagCode::kPlaceObject3: {
        Place place;
        if (ReadPlace(code, body, swf_version, place))
          frame.places.push_back(base::move(place));
        break;
      }
      case TagCode::kRemoveObject: {
        Reader rr(body);
        rr.U16();  // character id
        frame.removes.push_back(rr.U16());
        break;
      }
      case TagCode::kRemoveObject2: {
        Reader rr(body);
        frame.removes.push_back(rr.U16());
        break;
      }
      case TagCode::kFrameLabel: {
        Reader rr(body);
        frame.label = rr.Str();
        break;
      }
      case TagCode::kDoAction: {
        Script script;
        script.kind = Script::Kind::kFrame;
        script.timeline_id = timeline_id;
        script.frame = frame_index;
        script.code = body;
        movie.scripts.push_back(base::move(script));
        break;
      }
      case TagCode::kDoInitAction: {
        Reader rr(body);
        Script script;
        script.kind = Script::Kind::kInit;
        script.timeline_id = timeline_id;
        script.sprite_id = rr.U16();
        script.frame = frame_index;
        script.code = body.subspan(2);
        movie.scripts.push_back(base::move(script));
        break;
      }
      case TagCode::kDefineSprite:
        // Nested definitions were already collected by the definition pass, and
        // a sprite tag never appears inside another sprite's control stream.
        break;
      default:
        break;
    }
  }

  if (!frame.places.empty() || !frame.removes.empty() || !frame.label.empty())
    out.frames.push_back(base::move(frame));
}

}  // namespace

}  // namespace rx::swf
