// Builds a SWF byte for byte in memory and asserts what the readers give back,
// so the whole chain (container, shape, text, dictionary, timeline, ugui
// translation) is covered without an installed game.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstring>

#include "components/swf/abc.h"
#include "components/swf/bridge.h"
#include "components/swf/stage.h"
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

  {
    // InitArray takes its elements off the stack the same way a call takes its
    // arguments: element 0 on top. Reversing it silently swapped every pair the
    // scripts build, which is how GameDelegate's [scope, method] came back as
    // [method, scope] and no callback the game sent ever reached its handler.
    swf::Vm vm;
    base::Vector<rx::u8> prog;
    auto emit_str = [&prog](const char* literal) {
      prog.push_back(0x96);
      const rx::u16 len = static_cast<rx::u16>(std::strlen(literal) + 2);
      prog.push_back(static_cast<rx::u8>(len & 0xff));
      prog.push_back(static_cast<rx::u8>(len >> 8));
      prog.push_back(0);
      for (const char* c = literal; *c; ++c)
        prog.push_back(static_cast<rx::u8>(*c));
      prog.push_back(0);
    };
    auto emit_int = [&prog](rx::i32 value) {
      prog.push_back(0x96);
      prog.push_back(5);
      prog.push_back(0);
      prog.push_back(7);
      for (int i = 0; i < 4; ++i)
        prog.push_back(static_cast<rx::u8>((static_cast<rx::u32>(value) >> (i * 8)) & 0xff));
    };
    // CallMethod pops the method name, then the target, then the argument
    // count, so they go on in the opposite order.
    emit_int(0);  // join takes no arguments
    emit_str("second");
    emit_str("first");
    emit_int(2);
    prog.push_back(0x42);  // InitArray -> ["first", "second"]
    emit_str("join");
    prog.push_back(0x52);  // CallMethod
    prog.push_back(0x26);  // Trace
    prog.push_back(0x00);
    const rx::u32 script = vm.AddScript(ByteSpan{prog.data(), prog.size()});
    vm.Run(script, swf::AsValue::Undefined());
    Check(vm.traces().size() == 1 && vm.traces()[0] == "first,second",
          "InitArray keeps the elements in source order");
  }

  {
    // splice on an array whose length was never set. The components do exactly
    // this (BSScrollingList::ClearList splices its entry array before anything
    // has filled it), and reading that undefined length as an index used to be
    // undefined behaviour that left the array's length NaN.
    swf::Vm vm;
    const swf::AsValue list = swf::AsValue::Obj(vm.NewArray());
    vm.Get(list.object()).props.erase(base::String("length"));
    base::Vector<swf::AsValue> args;
    args.push_back(swf::AsValue::Number(0));
    args.push_back(vm.GetMember(list, "length"));
    vm.Call(vm.GetMember(list, "splice"), list, args);
    Check(vm.ToNumber(vm.GetMember(list, "length")) == 0,
          "splicing an array with no length leaves it empty, not NaN");

    base::Vector<swf::AsValue> one;
    one.push_back(swf::AsValue::Str("a"));
    vm.Call(vm.GetMember(list, "push"), list, one);
    one[0] = swf::AsValue::Str("b");
    vm.Call(vm.GetMember(list, "push"), list, one);
    one[0] = swf::AsValue::Str("c");
    vm.Call(vm.GetMember(list, "push"), list, one);
    base::Vector<swf::AsValue> cut;
    cut.push_back(swf::AsValue::Number(1));
    cut.push_back(swf::AsValue::Number(1));
    const swf::AsValue removed = vm.Call(vm.GetMember(list, "splice"), list, cut);
    Check(vm.ToString(vm.GetMember(removed, "0")) == "b", "splice returns what it removed");
    Check(vm.ToNumber(vm.GetMember(list, "length")) == 2 &&
              vm.ToString(vm.GetMember(list, "1")) == "c",
          "splice closes the gap it left");
    base::Vector<swf::AsValue> from;
    from.push_back(swf::AsValue::Number(1));
    const swf::AsValue tail = vm.Call(vm.GetMember(list, "slice"), list, from);
    Check(vm.ToNumber(vm.GetMember(tail, "length")) == 1 &&
              vm.ToString(vm.GetMember(tail, "0")) == "c",
          "slice copies the run it was asked for");
  }

  {
    // The host bridge. A call the host has no answer for queues up with the
    // response id the movie sent, which is what a host needs in order to answer
    // it later; one with an answer is settled on the spot.
    swf::Vm vm;
    swf::GameBridge bridge(vm);
    base::Vector<swf::AsValue> args;
    args.push_back(swf::AsValue::Number(7));  // the response id GameDelegate made
    args.push_back(swf::AsValue::Str("payload"));
    vm.DispatchExternal("RequestSaveList", args);
    Check(bridge.pending().size() == 1, "an unanswered call is queued for the host");
    Check(bridge.pending().size() == 1 && bridge.pending()[0].name == "RequestSaveList" &&
              bridge.pending()[0].id == 7 && bridge.pending()[0].args.size() == 1,
          "the queued call keeps its id and drops the id from the arguments");

    bridge.SetAnswer("GetPlatform", swf::AsValue::Number(0));
    bridge.ClearPending();
    vm.DispatchExternal("GetPlatform", args);
    Check(bridge.pending().empty(), "a call with a standing answer does not queue");
  }

  {
    // The timeline is what resolves a menu's states: each state lives on its
    // own frame of the same clip and the script switches with gotoAndStop, so
    // a frame-0 snapshot shows all of them at once. Built here as a Movie
    // directly - no SWF parsing in the way of what is being tested.
    swf::Movie movie;
    // Two sprites to place, ids 1 and 2.
    movie.sprites.push_back(swf::Timeline{});
    movie.sprites[0].id = 1;
    movie.sprites[0].frames.push_back(swf::Frame{});
    movie.sprites.push_back(swf::Timeline{});
    movie.sprites[1].id = 2;
    movie.sprites[1].frames.push_back(swf::Frame{});
    movie.characters[1] = swf::CharacterRef{swf::CharacterKind::kSprite, 0};
    movie.characters[2] = swf::CharacterRef{swf::CharacterKind::kSprite, 1};

    // Frame 0 places "normal"; frame 1 is labelled "Selected", clears it and
    // places "highlight" in its stead.
    swf::Frame first;
    swf::Place a;
    a.depth = 1;
    a.character_id = 1;
    a.has_character = true;
    a.name = "normal";
    first.places.push_back(base::move(a));
    movie.root.frames.push_back(base::move(first));

    swf::Frame second;
    second.label = "Selected";
    second.removes.push_back(1);
    swf::Place b;
    b.depth = 1;
    b.character_id = 2;
    b.has_character = true;
    b.name = "highlight";
    second.places.push_back(base::move(b));
    movie.root.frames.push_back(base::move(second));

    swf::Vm vm;
    swf::Stage stage(vm, movie);
    stage.Run();
    const swf::AsValue root = stage.root();
    Check(vm.GetMember(root, "normal").is_object(),
          "frame 0 places the clip that frame authored");
    Check(!vm.GetMember(root, "highlight").is_object(),
          "a later frame's clip is not present on frame 0");
    Check(vm.ToNumber(vm.GetMember(root, "_totalframes")) == 2,
          "the clip reports how many frames it has");

    Check(stage.GotoLabel(root, "Selected"), "a frame label resolves");
    Check(vm.GetMember(root, "highlight").is_object(),
          "the frame's own clip is placed on arrival");
    Check(!vm.GetMember(root, "normal").is_object(),
          "what the frame removes is gone");
    Check(vm.ToNumber(vm.GetMember(root, "_currentframe")) == 2,
          "_currentframe follows the goto");

    Check(stage.Goto(root, 0), "and back again");
    Check(vm.GetMember(root, "normal").is_object(), "the first frame's clip returns");
    Check(!vm.GetMember(root, "highlight").is_object(),
          "and the second frame's is dropped");
    Check(!stage.GotoLabel(root, "NoSuchLabel"),
          "an unknown label leaves the clip where it is");

    // play() steps a frame per tick and wraps, which is what a spinner is.
    stage.SetPlaying(root, true);
    stage.Tick(16.0);
    Check(vm.ToNumber(vm.GetMember(root, "_currentframe")) == 2,
          "a playing clip advances on a tick");
    stage.Tick(16.0);
    Check(vm.ToNumber(vm.GetMember(root, "_currentframe")) == 1,
          "and wraps at the end");
    stage.SetPlaying(root, false);
    stage.Tick(16.0);
    Check(vm.ToNumber(vm.GetMember(root, "_currentframe")) == 1,
          "a stopped clip stays put");
  }

  {
    // A clip carries the box the frame gave it. The menus lay themselves out
    // from these: a scrolling list divides its border's height by a row's to
    // decide how many rows fit, so a clip without a size shows nothing.
    swf::Movie movie;
    swf::Shape plate;
    plate.id = 1;
    plate.bounds = swf::Rect{0, 2000, 0, 600};  // 100x30 px
    movie.shapes.push_back(base::move(plate));
    movie.characters[1] = swf::CharacterRef{swf::CharacterKind::kShape, 0};

    swf::Timeline holder;
    holder.id = 2;
    swf::Frame inner;
    swf::Place art;
    art.depth = 1;
    art.character_id = 1;
    art.has_character = true;
    art.name = "art";
    inner.places.push_back(base::move(art));
    holder.frames.push_back(base::move(inner));
    movie.sprites.push_back(base::move(holder));
    movie.characters[2] = swf::CharacterRef{swf::CharacterKind::kSprite, 0};

    swf::Frame first;
    swf::Place placed;
    placed.depth = 1;
    placed.character_id = 2;
    placed.has_character = true;
    placed.name = "border";
    placed.matrix.translate_x = 400;  // 20 px
    placed.matrix.translate_y = 200;  // 10 px
    first.places.push_back(base::move(placed));
    movie.root.frames.push_back(base::move(first));

    swf::Vm vm;
    swf::Stage stage(vm, movie);
    stage.Run();
    const swf::AsValue border = vm.GetMember(stage.root(), "border");
    Check(vm.ToNumber(vm.GetMember(border, "_x")) == 20 &&
              vm.ToNumber(vm.GetMember(border, "_y")) == 10,
          "a clip sits where its matrix put it, in pixels");
    Check(vm.ToNumber(vm.GetMember(border, "_width")) == 100 &&
              vm.ToNumber(vm.GetMember(border, "_height")) == 30,
          "a sprite is as big as what it places");
  }

  {
    // Load events wait for the frame's own actions. A menu's onLoad calls the
    // helpers the root script installs (TextField.prototype.SetText is the one
    // that matters: it is how every list writes a row's label), so dispatching
    // during the build instead left every list filled with its placeholders.
    base::Vector<rx::u8> body;  // trace("loaded")
    body.push_back(0x96);
    body.push_back(static_cast<rx::u8>(std::strlen("loaded") + 2));
    body.push_back(0);
    body.push_back(0);
    for (const char* c = "loaded"; *c; ++c)
      body.push_back(static_cast<rx::u8>(*c));
    body.push_back(0);
    body.push_back(0x26);  // Trace

    base::Vector<rx::u8> code;
    auto literal = [&code](const char* text) {
      code.push_back(0x96);
      const rx::u16 len = static_cast<rx::u16>(std::strlen(text) + 2);
      code.push_back(static_cast<rx::u8>(len & 0xff));
      code.push_back(static_cast<rx::u8>(len >> 8));
      code.push_back(0);
      for (const char* c = text; *c; ++c)
        code.push_back(static_cast<rx::u8>(*c));
      code.push_back(0);
    };
    literal("MovieClip");
    code.push_back(0x1c);  // GetVariable
    literal("prototype");
    code.push_back(0x4e);  // GetMember
    literal("onLoad");
    // DefineFunction "" with no parameters, body appended after the action.
    code.push_back(0x9b);
    const rx::u16 payload = 1 + 2 + 2;
    code.push_back(static_cast<rx::u8>(payload & 0xff));
    code.push_back(static_cast<rx::u8>(payload >> 8));
    code.push_back(0);  // empty name: the function is pushed, not declared
    code.push_back(0);  // parameter count
    code.push_back(0);
    code.push_back(static_cast<rx::u8>(body.size() & 0xff));
    code.push_back(static_cast<rx::u8>(body.size() >> 8));
    for (rx::u8 byte : body)
      code.push_back(byte);
    code.push_back(0x4f);  // SetMember
    code.push_back(0x00);  // End

    swf::Movie movie;
    movie.sprites.push_back(swf::Timeline{});
    movie.sprites[0].id = 1;
    movie.sprites[0].frames.push_back(swf::Frame{});
    movie.characters[1] = swf::CharacterRef{swf::CharacterKind::kSprite, 0};
    swf::Frame frame;
    swf::Place place;
    place.depth = 1;
    place.character_id = 1;
    place.has_character = true;
    place.name = "child";
    frame.places.push_back(base::move(place));
    movie.root.frames.push_back(base::move(frame));
    swf::Script script;
    script.kind = swf::Script::Kind::kFrame;
    script.code = ByteSpan{code.data(), code.size()};
    movie.scripts.push_back(base::move(script));

    swf::Vm vm;
    swf::Stage stage(vm, movie);
    stage.Run();
    Check(vm.traces().size() == 2,
          "onLoad reaches every clip, after the root's own actions ran");
  }

  {
    // Try/Catch/Finally. The three blocks sit inline after the action, so a
    // machine that falls through runs all three: the catch block stores an
    // exception nobody threw and the stack is out of step for everything after
    // it, which is how a constructor stopped halfway through its own body.
    base::Vector<rx::u8> code;
    auto literal = [&code](const char* text) {
      code.push_back(0x96);
      const rx::u16 len = static_cast<rx::u16>(std::strlen(text) + 2);
      code.push_back(static_cast<rx::u8>(len & 0xff));
      code.push_back(static_cast<rx::u8>(len >> 8));
      code.push_back(0);
      for (const char* c = text; *c; ++c)
        code.push_back(static_cast<rx::u8>(*c));
      code.push_back(0);
    };
    // The three bodies, assembled first so their sizes are known.
    base::Vector<rx::u8> body;
    base::Vector<rx::u8> katch;
    base::Vector<rx::u8> after;
    {
      base::Vector<rx::u8>* into = &body;
      auto trace_into = [&into](const char* text) {
        into->push_back(0x96);
        into->push_back(static_cast<rx::u8>(std::strlen(text) + 2));
        into->push_back(0);
        into->push_back(0);
        for (const char* c = text; *c; ++c)
          into->push_back(static_cast<rx::u8>(*c));
        into->push_back(0);
        into->push_back(0x26);
      };
      trace_into("tried");
      into = &katch;
      trace_into("caught");
      into = &after;
      trace_into("after");
    }

    code.push_back(0x8f);  // Try
    const rx::u16 payload = 1 + 2 + 2 + 2 + 1;
    code.push_back(static_cast<rx::u8>(payload & 0xff));
    code.push_back(static_cast<rx::u8>(payload >> 8));
    code.push_back(0x04);  // catch stores to a register rather than a name
    code.push_back(static_cast<rx::u8>(body.size() & 0xff));
    code.push_back(static_cast<rx::u8>(body.size() >> 8));
    code.push_back(static_cast<rx::u8>(katch.size() & 0xff));
    code.push_back(static_cast<rx::u8>(katch.size() >> 8));
    code.push_back(0);  // no finally block
    code.push_back(0);
    code.push_back(1);  // the register the exception would land in
    for (rx::u8 byte : body)
      code.push_back(byte);
    for (rx::u8 byte : katch)
      code.push_back(byte);
    for (rx::u8 byte : after)
      code.push_back(byte);
    code.push_back(0x00);
    (void)literal;

    swf::Vm vm;
    const rx::u32 script = vm.AddScript(ByteSpan{code.data(), code.size()});
    vm.Run(script, swf::AsValue::Undefined());
    Check(vm.traces().size() == 2, "a try that completes runs its body and moves on");
    Check(vm.traces().size() == 2 && vm.traces()[0] == "tried" &&
              vm.traces()[1] == "after",
          "and the catch block is not run when nothing threw");
  }

  {
    // A clip's states come out as sibling groups, one per labelled frame that
    // puts something different on the display list. Without them the widgets
    // only ever stand for the frame the export was taken at, and a menu that
    // switches state with gotoAndStop has nothing to show.
    swf::Movie movie;
    swf::Shape normal;
    normal.id = 1;
    normal.bounds = swf::Rect{0, 400, 0, 200};
    movie.shapes.push_back(base::move(normal));
    swf::Shape selected;
    selected.id = 2;
    selected.bounds = swf::Rect{0, 400, 0, 400};
    movie.shapes.push_back(base::move(selected));
    movie.characters[1] = swf::CharacterRef{swf::CharacterKind::kShape, 0};
    movie.characters[2] = swf::CharacterRef{swf::CharacterKind::kShape, 1};

    swf::Timeline row;
    row.id = 3;
    swf::Frame plain;
    plain.label = "Normal";
    swf::Place art;
    art.depth = 1;
    art.character_id = 1;
    art.has_character = true;
    art.name = "art";
    plain.places.push_back(base::move(art));
    row.frames.push_back(base::move(plain));
    // A tween: labelled, but placing the same thing. Faders are made of these,
    // and counting them would triple the whole menu underneath one.
    swf::Frame fading;
    fading.label = "fadeIn";
    swf::Place moved;
    moved.depth = 1;
    moved.move = true;
    moved.has_color_transform = true;
    moved.color_transform.mul_a = 0.5f;
    fading.places.push_back(base::move(moved));
    row.frames.push_back(base::move(fading));
    swf::Frame highlit;
    highlit.label = "Selected";
    swf::Place bigger;
    bigger.depth = 1;
    bigger.character_id = 2;
    bigger.has_character = true;
    bigger.name = "art";
    highlit.places.push_back(base::move(bigger));
    row.frames.push_back(base::move(highlit));
    movie.sprites.push_back(base::move(row));
    movie.characters[3] = swf::CharacterRef{swf::CharacterKind::kSprite, 0};

    swf::Frame stage;
    swf::Place placed;
    placed.depth = 1;
    placed.character_id = 3;
    placed.has_character = true;
    placed.name = "Entry0";
    stage.places.push_back(base::move(placed));
    movie.root.frames.push_back(base::move(stage));

    swf::UguiExportOptions options;
    options.name = "states";
    const swf::UguiScreen screen = swf::ExportUgui(movie, options);
    Check(Contains(screen.markup, "Entry0__state0"), "the first state is a group");
    Check(Contains(screen.markup, "Entry0__state2"),
          "and so is the frame that places something else");
    Check(!Contains(screen.markup, "Entry0__state1"),
          "but a tween is not a state: it places the same thing");

    options.max_states = 1;
    const swf::UguiScreen flat = swf::ExportUgui(movie, options);
    Check(!Contains(flat.markup, "__state"),
          "and one state per clip is the old single-frame translation");
  }

  {
    // A bare assignment on a timeline names that timeline's own property, not a
    // global. That is how a component's authored parameters reach the clip they
    // were placed on: a tab's label is a `construct` handler doing `labelID =
    // "$QUESTS"`, and sending it to _global left every tab reading "BUTTON
    // TEXT". Also the shape of the whole clip-event path, so it is checked by
    // running one against a clip.
    base::Vector<rx::u8> code;
    code.push_back(0x96);  // Push "labelID"
    code.push_back(static_cast<rx::u8>(std::strlen("labelID") + 2));
    code.push_back(0);
    code.push_back(0);
    for (const char* c = "labelID"; *c; ++c)
      code.push_back(static_cast<rx::u8>(*c));
    code.push_back(0);
    code.push_back(0x96);  // Push "$QUESTS"
    code.push_back(static_cast<rx::u8>(std::strlen("$QUESTS") + 2));
    code.push_back(0);
    code.push_back(0);
    for (const char* c = "$QUESTS"; *c; ++c)
      code.push_back(static_cast<rx::u8>(*c));
    code.push_back(0);
    code.push_back(0x1d);  // SetVariable
    code.push_back(0x00);

    swf::Movie movie;
    movie.sprites.push_back(swf::Timeline{});
    movie.sprites[0].id = 1;
    movie.sprites[0].frames.push_back(swf::Frame{});
    movie.characters[1] = swf::CharacterRef{swf::CharacterKind::kSprite, 0};
    swf::Frame frame;
    swf::Place place;
    place.depth = 1;
    place.character_id = 1;
    place.has_character = true;
    place.name = "QuestsTab";
    place.clip_event_flags.push_back(0x00040000u);  // construct
    place.clip_event_code.push_back(ByteSpan{code.data(), code.size()});
    frame.places.push_back(base::move(place));
    movie.root.frames.push_back(base::move(frame));

    swf::Vm vm;
    swf::Stage stage(vm, movie);
    stage.set_authored_state(true);
    stage.Run();
    const swf::AsValue tab = vm.GetMember(stage.root(), "QuestsTab");
    Check(vm.ToString(vm.GetMember(tab, "labelID")) == "$QUESTS",
          "a placement's construct handler sets the clip's own property");
    Check(vm.GetMember(swf::AsValue::Obj(vm.global()), "labelID").is_undefined(),
          "and not a global");

    // Off by default: the translation carries one frame, and these move clips
    // into states it has no widgets for.
    swf::Vm plain_vm;
    swf::Stage plain(plain_vm, movie);
    plain.Run();
    Check(plain_vm.GetMember(plain_vm.GetMember(plain.root(), "QuestsTab"), "labelID")
              .is_undefined(),
          "authored state is opt-in");
  }

  {
    // The host bridge answered while the movie is still asking. GameDelegate
    // drops its response slot the moment the call returns, so an answer a frame
    // later answers nothing; the answerer runs inside the call instead.
    swf::Vm vm;
    swf::GameBridge bridge(vm);
    static int asked = 0;
    asked = 0;
    bridge.set_answerer(
        [](void*, swf::GameBridge&, const swf::GameBridge::Call& call) {
          if (call.name != "RequestPlayerInfo")
            return false;
          ++asked;
          return true;
        },
        nullptr);
    base::Vector<swf::AsValue> args;
    args.push_back(swf::AsValue::Number(3));
    vm.DispatchExternal("RequestPlayerInfo", args);
    vm.DispatchExternal("PlaySound", args);
    Check(asked == 1, "the answerer sees the call as the movie makes it");
    Check(bridge.pending().size() == 1 && bridge.pending()[0].name == "PlaySound",
          "what it declines still queues for the host to look at");
  }


  {
    // addProperty backs a name with functions instead of a slot. The shipped
    // components use it for nearly every public field, and the getter has to
    // run against the object the read STARTED from, not the prototype that
    // carries the accessor.
    swf::Vm vm;
    const swf::AsValue global = swf::AsValue::Obj(vm.global());
    const swf::AsValue object_proto =
        vm.GetMember(vm.GetMember(global, "Object"), "prototype");
    const rx::u32 proto = vm.NewObject(object_proto.object());

    const rx::u32 getter = vm.NewNative(
        [](swf::Vm& v, const swf::AsValue& self, const base::Vector<swf::AsValue>&) {
          return v.GetMember(self, "marker");
        });
    const rx::u32 setter = vm.NewNative(
        [](swf::Vm& v, const swf::AsValue& self, const base::Vector<swf::AsValue>& a) {
          v.SetMember(self, "marker", a.empty() ? swf::AsValue::Undefined() : a[0]);
          return swf::AsValue::Undefined();
        });

    base::Vector<swf::AsValue> args;
    args.push_back(swf::AsValue::Str("thing"));
    args.push_back(swf::AsValue::Obj(getter));
    args.push_back(swf::AsValue::Obj(setter));
    vm.Call(vm.GetMember(swf::AsValue::Obj(proto), "addProperty"),
            swf::AsValue::Obj(proto), args);

    const swf::AsValue instance = swf::AsValue::Obj(vm.NewObject(proto));
    vm.SetMember(instance, "marker", swf::AsValue::Str("mine"));
    Check(vm.ToString(vm.GetMember(instance, "thing")) == "mine",
          "an inherited accessor reads against the instance");
    vm.SetMember(instance, "thing", swf::AsValue::Str("written"));
    Check(vm.ToString(vm.GetMember(instance, "marker")) == "written",
          "and writes through its setter");
  }


  {
    // attachMovie builds a clip from an exported symbol at runtime. The lists
    // that fill themselves use it 414 times across the shipped scripts, always
    // paired with getNextHighestDepth.
    swf::Movie movie;
    movie.sprites.push_back(swf::Timeline{});
    movie.sprites[0].id = 7;
    movie.sprites[0].frames.push_back(swf::Frame{});
    movie.characters[7] = swf::CharacterRef{swf::CharacterKind::kSprite, 0};
    movie.exports[7] = base::String("RowSymbol");
    movie.root.frames.push_back(swf::Frame{});

    swf::Vm vm;
    swf::Stage stage(vm, movie);
    stage.Run();
    const swf::AsValue root = stage.root();

    Check(stage.NextDepth(root) == 1, "an empty clip hands out the first depth");
    const swf::AsValue row =
        stage.Attach(root, "RowSymbol", "Entry0", stage.NextDepth(root));
    Check(row.is_object(), "attaching an exported symbol yields a clip");
    Check(vm.GetMember(root, "Entry0").is_object(),
          "and it is reachable from the parent by name");
    Check(vm.ToString(vm.GetMember(row, "_name")) == "Entry0",
          "the attached clip takes the name it was given");
    Check(stage.NextDepth(root) == 2, "the depth counter climbs past it");

    Check(!stage.Attach(root, "NoSuchSymbol", "x", 3).is_object(),
          "a symbol the movie does not export attaches nothing");

    Check(stage.Remove(row), "a clip can be taken back out");
    Check(!vm.GetMember(root, "Entry0").is_object(),
          "and is no longer reachable from its parent");

    const swf::AsValue holder = stage.CreateEmpty(root, "Holder", 9);
    Check(holder.is_object() && vm.GetMember(root, "Holder").is_object(),
          "an empty clip can be created to hold things");

    const swf::AsValue again =
        stage.Attach(root, "RowSymbol", "Entry1", stage.NextDepth(root));
    const swf::AsValue copy = stage.Duplicate(again, "Entry2", stage.NextDepth(root));
    Check(copy.is_object() && vm.GetMember(root, "Entry2").is_object(),
          "a clip can be duplicated beside itself");
    Check(vm.ToString(vm.GetMember(copy, "_name")) == "Entry2",
          "and the copy takes the new name");
  }


  {
    // A placed text field has to inherit the TextField prototype, because the
    // shipped scripts hang their own helpers on it (SetText is theirs, used 659
    // times) and measure through getLineMetrics when they lay a bar out.
    swf::Movie movie;
    swf::EditText field;
    field.id = 4;
    field.initial_text = "hello";
    movie.edit_texts.push_back(base::move(field));
    movie.characters[4] = swf::CharacterRef{swf::CharacterKind::kEditText, 0};
    swf::Frame frame;
    swf::Place place;
    place.depth = 1;
    place.character_id = 4;
    place.has_character = true;
    place.name = "label";
    frame.places.push_back(base::move(place));
    movie.root.frames.push_back(base::move(frame));

    swf::Vm vm;
    swf::Stage stage(vm, movie);
    stage.Run();
    const swf::AsValue label = vm.GetMember(stage.root(), "label");
    Check(label.is_object(), "the text field is placed and named");
    Check(vm.ToString(vm.GetMember(label, "text")) == "hello",
          "it carries the text the tag authored");

    // Reached through the prototype, not set on the field itself.
    const swf::AsValue metrics =
        vm.Call(vm.GetMember(label, "getLineMetrics"), label, base::Vector<swf::AsValue>());
    Check(metrics.is_object() && vm.ToNumber(vm.GetMember(metrics, "width")) > 0,
          "a field measures itself through the TextField prototype");

    // A script's own extension of the prototype reaches every field.
    vm.SetMember(swf::AsValue::Obj(vm.text_field_prototype()), "Shout",
                 swf::AsValue::Obj(vm.NewNative(
                     [](swf::Vm& v, const swf::AsValue& self,
                        const base::Vector<swf::AsValue>&) {
                       return v.GetMember(self, "text");
                     })));
    Check(vm.ToString(vm.Call(vm.GetMember(label, "Shout"), label,
                              base::Vector<swf::AsValue>())) == "hello",
          "and a helper added to the prototype binds to the field");
  }


  {
    // Timers: a menu sets them for key repeat, fades and deferred setup, so a
    // run that never ticks leaves that work queued forever.
    swf::Vm vm;
    const rx::u32 counter = vm.NewObject();
    vm.SetMember(swf::AsValue::Obj(counter), "n", swf::AsValue::Number(0));
    const rx::u32 bump = vm.NewNative(
        [](swf::Vm& v, const swf::AsValue& self, const base::Vector<swf::AsValue>&) {
          v.SetMember(self, "n", swf::AsValue::Number(v.ToNumber(v.GetMember(self, "n")) + 1));
          return swf::AsValue::Undefined();
        });
    const rx::u32 id = vm.AddTimer(swf::AsValue::Obj(bump), swf::AsValue::Obj(counter),
                                   base::Vector<swf::AsValue>(), 100.0, false);
    Check(vm.Tick(50.0) == 0, "a timer does not fire before it is due");
    Check(vm.Tick(60.0) == 1, "and does once it is");
    Check(vm.ToNumber(vm.GetMember(swf::AsValue::Obj(counter), "n")) == 1,
          "the handler ran against the scope it was given");
    Check(vm.Tick(100.0) == 1, "an interval repeats");
    vm.ClearTimer(id);
    Check(vm.Tick(100.0) == 0, "and stops once cleared");

    const rx::u32 once = vm.AddTimer(swf::AsValue::Obj(bump), swf::AsValue::Obj(counter),
                                     base::Vector<swf::AsValue>(), 10.0, true);
    (void)once;
    Check(vm.Tick(20.0) == 1, "a one-shot fires");
    Check(vm.Tick(100.0) == 0, "and does not come back");
  }
  {
    // Events. onLoad goes out after the constructor, because that is where a
    // class installs its own handler; onEnterFrame is broadcast per frame.
    swf::Movie movie;
    movie.sprites.push_back(swf::Timeline{});
    movie.sprites[0].id = 3;
    movie.sprites[0].frames.push_back(swf::Frame{});
    movie.characters[3] = swf::CharacterRef{swf::CharacterKind::kSprite, 0};
    swf::Frame frame;
    swf::Place place;
    place.depth = 1;
    place.character_id = 3;
    place.has_character = true;
    place.name = "child";
    frame.places.push_back(base::move(place));
    movie.root.frames.push_back(base::move(frame));

    swf::Vm vm;
    swf::Stage stage(vm, movie);
    stage.Run();
    const swf::AsValue child = vm.GetMember(stage.root(), "child");
    Check(child.is_object(), "the child clip exists");

    vm.SetMember(child, "ticks", swf::AsValue::Number(0));
    vm.SetMember(child, "onEnterFrame",
                 swf::AsValue::Obj(vm.NewNative(
                     [](swf::Vm& v, const swf::AsValue& self,
                        const base::Vector<swf::AsValue>&) {
                       v.SetMember(self, "ticks",
                                   swf::AsValue::Number(
                                       v.ToNumber(v.GetMember(self, "ticks")) + 1));
                       return swf::AsValue::Undefined();
                     })));
    stage.Tick(16.0);
    stage.Tick(16.0);
    Check(vm.ToNumber(vm.GetMember(child, "ticks")) == 2,
          "onEnterFrame reaches a clip once per frame");
    Check(!stage.Dispatch(child, "onNothingLikeThis"),
          "dispatching a handler a clip does not carry does nothing");
  }


  {
    // Scaleform's .gfx export moves every bitmap out to a file beside the movie
    // and leaves a tag naming it. Skyrim's quest_journal.gfx carries 163 of
    // these where the .swf twin carries 163 bitmaps, so a reader that ignores
    // them loses all of that movie's raster art.
    swf::Movie movie;
    swf::ExternalImage image;
    image.id = 5;
    image.width = 88;
    image.height = 64;
    image.name = "360_Start.png";
    image.file = "360_Start.png.dds";
    movie.external_images.push_back(base::move(image));
    movie.characters[5] = swf::CharacterRef{swf::CharacterKind::kExternalImage, 0};
    swf::Frame frame;
    swf::Place place;
    place.depth = 1;
    place.character_id = 5;
    place.has_character = true;
    place.name = "glyph";
    frame.places.push_back(base::move(place));
    movie.root.frames.push_back(base::move(frame));

    Check(movie.FindExternalImage(5) != nullptr, "an external image resolves by id");

    swf::UguiExportOptions options;
    options.name = "hud";
    const swf::UguiScreen screen = swf::ExportUgui(movie, options);
    Check(Contains(screen.markup, "image glyph"),
          "a placed external image becomes an image widget");
    Check(Contains(screen.markup, "width: 88px"),
          "sized from the tag, since there are no pixels to measure");
    bool recorded = false;
    for (const swf::ExportedAsset& asset : screen.assets)
      if (asset.source == "360_Start.png.dds")
        recorded = true;
    Check(recorded, "and the asset records the file to fetch beside the movie");
  }

  std::printf("swftest: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
