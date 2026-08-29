// Builds a SWF byte for byte in memory and asserts what the readers give back,
// so the whole chain (container, shape, text, dictionary, timeline, ugui
// translation) is covered without an installed game.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstring>

#include "components/swf/abc.h"
#include "components/swf/vm.h"
#include "components/swf/movie.h"
#include "components/swf/shape.h"
#include "components/swf/swf.h"
#include "components/swf/ugui_export.h"

using namespace rx;

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
  if (condition)
    return;
  std::printf("FAIL: %s\n", what);
  ++failures;
}

// Mirror of swf::Reader: big-endian bit packing, MSB first, byte aligned on
// demand.
class Writer {
 public:
  void Bits(u32 value, u32 count) {
    for (u32 i = 0; i < count; ++i) {
      const u32 bit = (value >> (count - 1 - i)) & 1u;
      if (bit_ == 0)
        bytes_.push_back(0);
      bytes_[bytes_.size() - 1] |= static_cast<u8>(bit << (7 - bit_));
      bit_ = (bit_ + 1) % 8;
    }
  }

  void SignedBits(i32 value, u32 count) {
    Bits(static_cast<u32>(value) & ((count >= 32) ? 0xffffffffu : ((1u << count) - 1)),
         count);
  }

  void Align() { bit_ = 0; }

  void U8(u8 value) {
    Align();
    bytes_.push_back(value);
  }

  void U16(u16 value) {
    U8(static_cast<u8>(value & 0xff));
    U8(static_cast<u8>(value >> 8));
  }

  void U32(u32 value) {
    U16(static_cast<u16>(value & 0xffff));
    U16(static_cast<u16>(value >> 16));
  }

  void Str(const char* text) {
    for (const char* p = text; *p; ++p)
      U8(static_cast<u8>(*p));
    U8(0);
  }

  void Rect(i32 x_min, i32 x_max, i32 y_min, i32 y_max) {
    Align();
    Bits(18, 5);
    SignedBits(x_min, 18);
    SignedBits(x_max, 18);
    SignedBits(y_min, 18);
    SignedBits(y_max, 18);
    Align();
  }

  // Translation only, which is all the timeline in this test needs.
  void Matrix(i32 tx, i32 ty) {
    Align();
    Bits(0, 1);  // no scale
    Bits(0, 1);  // no rotate
    Bits(18, 5);
    SignedBits(tx, 18);
    SignedBits(ty, 18);
    Align();
  }

  void Append(const Writer& other) {
    Align();
    for (mem_size i = 0; i < other.bytes_.size(); ++i)
      bytes_.push_back(other.bytes_[i]);
  }

  void Tag(u16 code, const Writer& body) {
    Align();
    if (body.size() < 0x3f) {
      U16(static_cast<u16>((code << 6) | body.size()));
    } else {
      U16(static_cast<u16>((code << 6) | 0x3f));
      U32(static_cast<u32>(body.size()));
    }
    Append(body);
  }

  mem_size size() const { return bytes_.size(); }
  const base::Vector<u8>& bytes() const { return bytes_; }
  base::Vector<u8>& bytes() { return bytes_; }

 private:
  base::Vector<u8> bytes_;
  u32 bit_ = 0;
};

// A 100x50 pixel rectangle filled with one solid colour, the shape every
// Scaleform backing plate is.
Writer SolidRectShape(u16 id, u8 r, u8 g, u8 b) {
  Writer w;
  w.U16(id);
  w.Rect(0, 2000, 0, 1000);

  w.U8(1);  // one fill style
  w.U8(0);  // solid
  w.U8(r);
  w.U8(g);
  w.U8(b);
  w.U8(0);  // no line styles

  w.Bits(1, 4);  // fill index bits
  w.Bits(0, 4);  // line index bits

  // Move to the origin and select the fill.
  w.Bits(0, 1);  // non-edge
  w.Bits(0, 1);  // no new styles
  w.Bits(0, 1);  // no line style
  w.Bits(1, 1);  // fill style 1
  w.Bits(0, 1);  // no fill style 0
  w.Bits(1, 1);  // move to
  w.Bits(18, 5);
  w.SignedBits(0, 18);
  w.SignedBits(0, 18);
  w.Bits(1, 1);  // fill style 1 index

  const i32 deltas[4][2] = {{2000, 0}, {0, 1000}, {-2000, 0}, {0, -1000}};
  for (const auto& d : deltas) {
    w.Bits(1, 1);   // edge
    w.Bits(1, 1);   // straight
    w.Bits(15, 4);  // 17 bits per coordinate (the field holds bits - 2)
    w.Bits(1, 1);   // general line
    w.SignedBits(d[0], 17);
    w.SignedBits(d[1], 17);
  }

  w.Bits(0, 6);  // end shape record
  w.Align();
  return w;
}

Writer EditText(u16 id) {
  Writer w;
  w.U16(id);
  w.Rect(0, 4000, 0, 400);
  w.Bits(1, 1);  // has text
  w.Bits(0, 1);  // word wrap
  w.Bits(0, 1);  // multiline
  w.Bits(0, 1);  // password
  w.Bits(1, 1);  // read only
  w.Bits(1, 1);  // has text colour
  w.Bits(0, 1);  // has max length
  w.Bits(1, 1);  // has font
  w.Bits(0, 1);  // has font class
  w.Bits(0, 1);  // auto size
  w.Bits(1, 1);  // has layout
  w.Bits(0, 1);  // no select
  w.Bits(0, 1);  // border
  w.Bits(0, 1);  // was static
  w.Bits(0, 1);  // html
  w.Bits(0, 1);  // use outlines
  w.Align();
  w.U16(7);    // font id
  w.U16(240);  // font height, 12 px
  w.U8(0xee);  // colour
  w.U8(0xcc);
  w.U8(0x22);
  w.U8(0xff);
  w.U8(2);   // align: centre
  w.U16(0);  // left margin
  w.U16(0);  // right margin
  w.U16(0);  // indent
  w.U16(0);  // leading
  w.Str("HealthPercent");
  w.Str("100");
  return w;
}

Writer Place(u16 depth, u16 character, const char* name, i32 tx, i32 ty) {
  Writer w;
  u8 flags = 0x02 | 0x04;  // has character, has matrix
  if (name)
    flags |= 0x20;
  w.U8(flags);
  w.U16(depth);
  w.U16(character);
  w.Matrix(tx, ty);
  if (name)
    w.Str(name);
  return w;
}

base::Vector<u8> BuildMovie() {
  Writer body;
  body.Rect(0, 1280 * 20, 0, 720 * 20);
  body.U16(24 * 256);  // frame rate, 8.8 fixed
  body.U16(1);         // frame count

  {
    Writer t;
    t.U8(0x11);  // background colour
    t.U8(0x22);
    t.U8(0x33);
    body.Tag(9, t);  // SetBackgroundColor
  }
  body.Tag(2, SolidRectShape(1, 0x40, 0x80, 0xc0));  // DefineShape
  body.Tag(37, EditText(2));                          // DefineEditText

  {
    // A sprite holding the shape and the text.
    Writer sprite;
    sprite.U16(3);  // sprite id
    sprite.U16(1);  // frame count
    sprite.Tag(26, Place(1, 1, "Plate", 0, 0));
    sprite.Tag(26, Place(2, 2, "HealthText", 200, 60));
    Writer empty;
    sprite.Tag(1, empty);  // ShowFrame
    sprite.Tag(0, empty);  // End
    body.Tag(39, sprite);  // DefineSprite
  }

  {
    Writer t;
    t.U16(1);  // one export
    t.U16(3);
    t.Str("HealthMeter");
    body.Tag(56, t);  // ExportAssets
  }

  body.Tag(26, Place(1, 3, "MeterInstance", 100 * 20, 50 * 20));
  Writer empty;
  body.Tag(1, empty);  // ShowFrame
  body.Tag(0, empty);  // End

  Writer file;
  file.U8('F');
  file.U8('W');
  file.U8('S');
  file.U8(6);
  file.U32(static_cast<u32>(8 + body.size()));
  file.Append(body);
  return base::move(file.bytes());
}

bool Contains(const base::String& haystack, const char* needle) {
  return haystack.find(needle) != base::String::npos;
}

}  // namespace

int main() {
  const base::Vector<u8> bytes = BuildMovie();
  auto file = swf::OpenSwf(ByteSpan{bytes.data(), bytes.size()});
  Check(file.has_value(), "the movie opens");
  if (!file.has_value())
    return 1;

  Check(file.value().version == 6, "version survives the header");
  Check(file.value().frame_size.width() == 1280 * 20, "stage width in twips");
  Check(file.value().frame_size.height() == 720 * 20, "stage height in twips");
  Check(file.value().frame_rate == 24.0f, "frame rate is 8.8 fixed point");
  Check(file.value().frame_count == 1, "frame count");

  auto movie = swf::LoadMovie(file.value());
  Check(movie.has_value(), "the movie decodes");
  if (!movie.has_value())
    return 1;
  const swf::Movie& m = movie.value();

  Check(m.background.r == 0x11 && m.background.g == 0x22 && m.background.b == 0x33,
        "background colour");
  Check(m.shapes.size() == 1, "one shape in the dictionary");
  Check(m.edit_texts.size() == 1, "one edit text in the dictionary");
  Check(m.sprites.size() == 1, "one sprite in the dictionary");

  const swf::Shape* shape = m.FindShape(1);
  Check(shape != nullptr, "the shape resolves by character id");
  if (shape) {
    Check(shape->fills.size() == 1, "the shape has one fill");
    swf::Rgba color;
    Check(swf::AsSolidRect(*shape, color), "the shape reads as a solid rectangle");
    Check(color.r == 0x40 && color.g == 0x80 && color.b == 0xc0, "the fill colour");
    Check(shape->bounds.width() == 2000 && shape->bounds.height() == 1000,
          "the shape bounds");
    Check(shape->fill_paths.size() == 1 && shape->fill_paths[0].contours.size() == 1,
          "the edges stitch into one contour");
  }

  const swf::EditText* text = m.FindEditText(2);
  Check(text != nullptr, "the edit text resolves by character id");
  if (text) {
    Check(text->variable == "HealthPercent", "the ActionScript variable binding");
    Check(text->initial_text == "100", "the initial text");
    Check(text->font_height == 240, "the font height in twips");
    Check(text->align == swf::TextAlign::kCenter, "the alignment");
    Check(text->color.r == 0xee && text->color.g == 0xcc && text->color.b == 0x22,
          "the text colour");
  }

  Check(m.ExportName(3) == "HealthMeter", "the export name of the sprite");

  const swf::Timeline* sprite = m.FindSprite(3);
  Check(sprite != nullptr, "the sprite resolves by character id");
  if (sprite) {
    Check(sprite->frames.size() == 1, "the sprite has one frame");
    Check(sprite->frames[0].places.size() == 2, "the sprite places two characters");
    Check(sprite->frames[0].places[0].name == "Plate", "the instance name");
  }

  Check(m.root.frames.size() == 1, "the root has one frame");
  Check(m.root.frames.size() == 1 && m.root.frames[0].places.size() == 1,
        "the root places the sprite");

  swf::UguiExportOptions options;
  options.name = "test";
  const swf::UguiScreen screen = swf::ExportUgui(m, options);
  Check(screen.widget_count >= 3, "the translation emits the root, shape and text");
  // Namespaced: a movie's stem collides with the host's own fragments, and two
  // roots with one name merge into a single tree (Fallout 4 ships mainmenu.swf
  // and recreation has main_menu.ugui).
  Check(Contains(screen.markup, "panel vanilla_test {"),
        "the root panel namespaces the screen name");
  Check(Contains(screen.markup, "MeterInstance"),
        "the instance name becomes the widget name");
  Check(Contains(screen.markup, "background: #4080c0"),
        "the solid rectangle becomes a panel background");
  Check(Contains(screen.markup, "text HealthText"), "the edit text becomes a text widget");
  Check(Contains(screen.markup, "text: \"100\""), "the initial text carries over");
  Check(Contains(screen.markup, "HealthPercent"),
        "the ActionScript binding is kept as a note");
  Check(Contains(screen.markup, "text-align: center"), "the alignment carries over");
  // The sprite sits at 100,50 px and the text 10,3 px inside it.
  Check(Contains(screen.markup, "left: 100px; top: 50px"),
        "the sprite lands where placed");
  // Every length carries its unit: ugui reads a bare decimal between 0 and 1 as
  // a flex fraction, which silently collapses the widget.
  Check(!Contains(screen.markup, "left: 0.") && !Contains(screen.markup, "top: 0.") &&
            !Contains(screen.markup, "width: 0.") &&
            !Contains(screen.markup, "height: 0."),
        "no unitless sub-pixel length reaches the markup");

  // AS3 lists ship empty and name their row class in the bytecode, so the
  // scanner that finds the wiring has to survive a body it cannot fully decode.
  {
    swf::AbcFile abc;
    Check(swf::ParseListBindings(abc).empty(), "no bindings without a DoABC block");
    // A body whose opcode stream desynchronises must stop, not run away.
    abc.strings.push_back("");
    abc.methods.push_back(swf::AbcMethod{});
    abc.methods[0].body = 0;
    static const u8 kGarbage[] = {0xff, 0xff, 0xff, 0xff};
    swf::AbcMethodBody body;
    body.method = 0;
    body.code = ByteSpan{kGarbage, sizeof(kGarbage)};
    abc.bodies.push_back(base::move(body));
    swf::AbcClass klass;
    klass.name = "Test";
    klass.constructor = 0;
    abc.classes.push_back(base::move(klass));
    Check(swf::ParseListBindings(abc).empty(), "an undecodable body yields no bindings");
  }


  // --- the interpreter ------------------------------------------------------
  // Hand-assembled AVM1: the disassembler is exercised elsewhere, so these
  // build bytecode directly and assert on what the machine computed.
  {
    auto push_string = [](base::Vector<rx::u8>& out, const char* text) {
      out.push_back(0x96);  // Push
      const rx::u16 len = static_cast<rx::u16>(std::strlen(text) + 2);
      out.push_back(static_cast<rx::u8>(len & 0xff));
      out.push_back(static_cast<rx::u8>(len >> 8));
      out.push_back(0);  // string
      for (const char* c = text; *c; ++c)
        out.push_back(static_cast<rx::u8>(*c));
      out.push_back(0);
    };
    auto push_double = [](base::Vector<rx::u8>& out, double value) {
      out.push_back(0x96);
      out.push_back(9);
      out.push_back(0);
      out.push_back(6);  // double
      rx::u8 raw[8];
      std::memcpy(raw, &value, 8);
      // AVM1 stores a double as two 32-bit halves, high word first.
      for (int i = 4; i < 8; ++i) out.push_back(raw[i]);
      for (int i = 0; i < 4; ++i) out.push_back(raw[i]);
    };

    // trace("a" + "b") and trace(2 + 3)
    base::Vector<rx::u8> code;
    push_string(code, "a");
    push_string(code, "b");
    code.push_back(0x47);  // Add2
    code.push_back(0x26);  // Trace
    push_double(code, 2);
    push_double(code, 3);
    code.push_back(0x47);  // Add2
    code.push_back(0x26);  // Trace
    code.push_back(0x00);  // End

    swf::Vm vm;
    const rx::u32 script = vm.AddScript(ByteSpan{code.data(), code.size()});
    vm.Run(script, swf::AsValue::Undefined());
    Check(vm.traces().size() == 2, "the interpreter reached both traces");
    Check(vm.traces().size() == 2 && vm.traces()[0] == "ab",
          "Add2 concatenates when either side is a string");
    Check(vm.traces().size() == 2 && vm.traces()[1] == "5",
          "Add2 adds when both sides are numbers");
    Check(!vm.exhausted(), "a finite script does not hit the step budget");
  }
  {
    // A jump backwards to itself is the shape a runaway loop takes; the budget
    // has to stop it rather than let it hang the caller.
    base::Vector<rx::u8> code;
    code.push_back(0x99);  // Jump, payload is one s16
    code.push_back(2);
    code.push_back(0);
    code.push_back(0xfb);  // -5: end (5) + -5 = 0, this same action
    code.push_back(0xff);
    code.push_back(0x00);
    swf::Vm vm;
    const rx::u32 script = vm.AddScript(ByteSpan{code.data(), code.size()});
    vm.Run(script, swf::AsValue::Undefined());
    Check(vm.exhausted(), "an endless script stops on the step budget");
  }
  {
    // The object model: a prototype chain resolves a member the instance does
    // not carry, which is what every one of these menus is built on.
    swf::Vm vm;
    const rx::u32 base_object = vm.NewObject();
    vm.SetMember(swf::AsValue::Obj(base_object), "kind", swf::AsValue::Str("base"));
    const rx::u32 derived = vm.NewObject(base_object);
    Check(vm.ToString(vm.GetMember(swf::AsValue::Obj(derived), "kind")) == "base",
          "a member resolves through the prototype chain");
    vm.SetMember(swf::AsValue::Obj(derived), "kind", swf::AsValue::Str("derived"));
    Check(vm.ToString(vm.GetMember(swf::AsValue::Obj(derived), "kind")) == "derived",
          "an own member shadows the prototype's");
  }


  {
    // GetVariable takes a PATH, not a bare name: the compiler emits
    // `Push "Shared.CenteredScrollingList"; GetVariable`. Resolving that as one
    // literal property name is why every class registration came back undefined.
    swf::Vm vm;
    const rx::u32 shared = vm.NewObject();
    vm.SetMember(swf::AsValue::Obj(vm.global()), "Shared", swf::AsValue::Obj(shared));
    vm.SetMember(swf::AsValue::Obj(shared), "List", swf::AsValue::Str("found"));

    base::Vector<rx::u8> code;
    code.push_back(0x96);  // Push "Shared.List"
    const char* path = "Shared.List";
    const rx::u16 len = static_cast<rx::u16>(std::strlen(path) + 2);
    code.push_back(static_cast<rx::u8>(len & 0xff));
    code.push_back(static_cast<rx::u8>(len >> 8));
    code.push_back(0);
    for (const char* c = path; *c; ++c)
      code.push_back(static_cast<rx::u8>(*c));
    code.push_back(0);
    code.push_back(0x1c);  // GetVariable
    code.push_back(0x26);  // Trace
    code.push_back(0x00);

    const rx::u32 script = vm.AddScript(ByteSpan{code.data(), code.size()});
    vm.Run(script, swf::AsValue::Undefined());
    Check(vm.traces().size() == 1 && vm.traces()[0] == "found",
          "GetVariable resolves a dotted path");
  }
  {
    // CallMethod with an undefined method name calls the target itself. That is
    // how `super()` and a function held in a variable are invoked; treating the
    // name as the string "undefined" silently skipped every base constructor.
    swf::Vm vm;
    const rx::u32 fn = vm.NewNative(
        [](swf::Vm&, const swf::AsValue&, const base::Vector<swf::AsValue>&) {
          return swf::AsValue::Str("ran");
        });
    vm.SetMember(swf::AsValue::Obj(vm.global()), "fn", swf::AsValue::Obj(fn));

    base::Vector<rx::u8> code;
    code.push_back(0x96);  // Push 0 (argument count) as an int
    code.push_back(5);
    code.push_back(0);
    code.push_back(7);
    for (int i = 0; i < 4; ++i)
      code.push_back(0);
    code.push_back(0x96);  // Push "fn"
    code.push_back(4);
    code.push_back(0);
    code.push_back(0);
    code.push_back('f');
    code.push_back('n');
    code.push_back(0);
    code.push_back(0x1c);  // GetVariable -> the function is the target
    code.push_back(0x96);  // Push undefined (the method name)
    code.push_back(1);
    code.push_back(0);
    code.push_back(3);
    code.push_back(0x52);  // CallMethod
    code.push_back(0x26);  // Trace
    code.push_back(0x00);

    const rx::u32 script = vm.AddScript(ByteSpan{code.data(), code.size()});
    vm.Run(script, swf::AsValue::Undefined());
    Check(vm.traces().size() == 1 && vm.traces()[0] == "ran",
          "CallMethod with no method name calls the target itself");
  }

  std::printf("swftest: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
