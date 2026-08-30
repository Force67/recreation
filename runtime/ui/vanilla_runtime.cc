#include "runtime/ui/vanilla_runtime.h"

#include <base/containers/pair.h>
#include <base/memory/move.h>
#include <base/option.h>

#if defined(RECREATION_HAS_UGUI)

#include <ugui/ultragui.h>
#include <ugui/widgets/text.h>
#include <ugui/widgets/widget.h>

#include <filesystem>
#include <fstream>

#include "components/swf/abc.h"
#include "components/swf/avm2.h"
#include "components/swf/bridge.h"
#include "components/swf/movie.h"
#include "components/swf/stage.h"
#include "components/swf/swf.h"
#include "components/swf/vm.h"
#include "core/log.h"

namespace rx::ui {
namespace {

namespace fs = std::filesystem;

// "no such entry" for the binding table, which is indexed from zero.
constexpr u32 kNoBinding = 0xffffffffu;

// Namespace scope so it registers before base::InitOptionsFromEnv runs.
base::Option<bool> VanillaVm{"ui.vanilla.vm", true, "RX_VANILLA_VM"};
// The parameters a placement authored and the actions on the frames a clip
// arrives at: a tab's label, a list's centring, the `stop()` that ends a fade.
// On, now that the translation carries the states these move a clip into. The
// switch is for telling a problem in them from one somewhere else.
base::Option<bool> VanillaAuthored{"ui.vanilla.authored", true, "RX_VANILLA_AUTHORED"};
// Logs what the ActionScript 3 binding resolved for every widget it drives.
// The screens are large and the state is spread over three trees, so this is
// how a panel that should be down and is not gets found.
base::Option<bool> VanillaReport{"ui.vanilla.report", false, "RX_VANILLA_REPORT"};
// A message to send an ActionScript 3 screen once it is up, for looking at a
// state the host has no input path to yet ("ReturnToMainState" is Fallout 4's
// way past its splash).
base::Option<const char*> VanillaSend{"ui.vanilla.send", nullptr, "RX_VANILLA_SEND"};

base::Vector<u8> ReadFile(const fs::path& path) {
  base::Vector<u8> out;
  std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
  if (!file)
    return out;
  const std::streamsize size = file.tellg();
  if (size <= 0)
    return out;
  file.seekg(0);
  out.resize(static_cast<mem_size>(size));
  file.read(reinterpret_cast<char*>(out.data()), size);
  return out;
}

// The exporter makes every widget name unique across the whole document, so the
// second `List_mc` it meets becomes `List_mc_2`. The interpreter still calls it
// `List_mc`, because that is its instance name inside its own parent. Matching
// the suffixed form is safe here: the search is within one parent's children,
// and a parent has at most one child that was originally called any given name.
bool NameMatches(base::StringRef want, base::StringRef actual) {
  if (want == actual)
    return true;
  if (actual.size() <= want.size() + 1)
    return false;
  for (mem_size i = 0; i < want.size(); ++i)
    if (actual[i] != want[i])
      return false;
  if (actual[want.size()] != '_')
    return false;
  for (mem_size i = want.size() + 1; i < actual.size(); ++i)
    if (actual[i] < '0' || actual[i] > '9')
      return false;
  return true;
}

// The frame a state group stands for, or -1 when the widget is not one. The
// exporter names them "<clip>__state<frame>", with the usual "_2" on a clash.
i32 StateFrameOf(base::StringRef name) {
  const char* marker = "__state";
  const mem_size len = 7;
  for (mem_size i = 0; i + len < name.size(); ++i) {
    bool hit = true;
    for (mem_size c = 0; c < len; ++c)
      hit = hit && name[i + c] == marker[c];
    if (!hit)
      continue;
    mem_size digit = i + len;
    if (digit >= name.size() || name[digit] < '0' || name[digit] > '9')
      return -1;
    i32 frame = 0;
    for (; digit < name.size() && name[digit] >= '0' && name[digit] <= '9'; ++digit)
      frame = frame * 10 + (name[digit] - '0');
    return frame;
  }
  return -1;
}

bool IsStateGroup(ugui::WidgetRegistry& world, ugui::wid widget) {
  const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(widget);
  return node && StateFrameOf(base::StringRef(node->name.c_str())) >= 0;
}

// The group a clip on `frame` is showing: the last one at or before it, since a
// state holds until the next one starts. Null when the widget has no groups,
// which is every clip whose timeline never changed what it placed.
ugui::wid ActiveState(ugui::WidgetRegistry& world, ugui::wid widget, i32 frame) {
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(widget);
  if (!h)
    return ugui::kNullWidget;
  ugui::wid best = ugui::kNullWidget;
  i32 best_frame = -1;
  for (ugui::wid child : h->children) {
    const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(child);
    if (!node)
      continue;
    const i32 at = StateFrameOf(base::StringRef(node->name.c_str()));
    if (at < 0 || at > frame || at <= best_frame)
      continue;
    best = child;
    best_frame = at;
  }
  return best;
}

// Every widget under `parent` that stands for the instance `name`. There is
// more than one when the clip has states: the translation emits a group per
// state and each carries its own copy, so a clip drives all of them and the
// inactive groups are simply not drawn.
void ChildrenNamed(ugui::WidgetRegistry& world, ugui::wid parent, base::StringRef name,
                   base::Vector<ugui::wid>& out) {
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(parent);
  if (!h)
    return;
  ugui::wid fallback = ugui::kNullWidget;
  for (ugui::wid child : h->children) {
    const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(child);
    if (!node)
      continue;
    const base::StringRef actual(node->name.c_str());
    // A state group is the translation's own scaffolding, not an instance the
    // movie ever named, so a lookup passes straight through it.
    if (StateFrameOf(actual) >= 0) {
      ChildrenNamed(world, child, name, out);
      continue;
    }
    if (name == actual) {
      out.push_back(child);  // an exact match always wins
      return;
    }
    if (!fallback.valid() && NameMatches(name, actual))
      fallback = child;
  }
  if (fallback.valid()) {
    out.push_back(fallback);
    return;
  }
  // Nothing at this level. A name can still stand for something further down:
  // an AS3 getter hands back a field several clips deep under the flat name the
  // class declares it as (`SplashScreenText_tf` lives inside
  // `SplashScreenHolder_mc`). Only reached when the level itself has no match,
  // so a name a parent does own always wins.
  for (ugui::wid child : h->children) {
    ChildrenNamed(world, child, name, out);
    if (!out.empty())
      return;
  }
}

}  // namespace

struct VanillaRuntime::Impl {
  // The movie's bytes outlive everything: the decoded tags and every script the
  // Vm runs are spans into this buffer.
  base::Vector<u8> bytes;
  swf::SwfFile file;
  swf::Movie movie;
  swf::Vm vm;
  base::UniquePointer<swf::GameBridge> bridge;
  base::UniquePointer<swf::Stage> stage;
  // The ActionScript 3 half. A movie carries one or the other, never both, so
  // whichever the movie has is the machine that runs it.
  base::Vector<swf::AbcFile> abc;
  base::UniquePointer<swf::Avm2> avm2;
  base::Vector<base::String> entries;

  // The ActionScript 3 objects that stand for widgets, and what each drives.
  struct As3Bound {
    u32 object = 0;
    ugui::wid widget;
    bool is_text = false;
    // The clip's own timeline, when the placement named one, and how opaque
    // the placement left it.
    const swf::Timeline* timeline = nullptr;
    f32 placed_alpha = 1.0f;
  };
  base::Vector<As3Bound> as3_bound;
  // The instance RunAbc built for each class. Binding has to reuse these rather
  // than construct its own: the opening messages went to these, and a screen
  // that has been told to open is the one holding the state to show.
  base::UnorderedMap<base::String, u32> as3_instances;

  // Walks the movie's root display list beside the widget tree. A placed sprite
  // whose character the movie bound a class to becomes an instance of it, the
  // way SymbolClass says, and that instance drives the widget the placement's
  // own name was translated into.
  void BindAbc(ugui::UIContext& ui, ugui::wid root_widget) {
    ugui::WidgetRegistry& world = ui.world();
    for (const swf::Place& place : swf::DisplayListAt(movie.root, 0)) {
      if (!place.has_character || place.name.empty())
        continue;
      const base::String* klass = movie.exports.find(place.character_id);
      if (klass == nullptr)
        continue;
      const u32* built = as3_instances.find(*klass);
      if (built == nullptr)
        continue;
      const swf::As3Value instance = swf::As3Value::Obj(*built);
      base::Vector<ugui::wid> widgets;
      ChildrenNamed(world, root_widget, place.name, widgets);
      for (ugui::wid widget : widgets)
        BindAbcInto(ui, instance, widget,
                    ChildRef{SpriteOf(place.character_id), place.character_id,
                             place.has_color_transform ? place.color_transform.mul_a : 1.0f},
                    0);
    }
  }

  // The timeline behind a character, when it has one.
  const swf::Timeline* SpriteOf(u16 character) const {
    const swf::CharacterRef* ref = movie.characters.find(character);
    if (!ref || ref->kind != swf::CharacterKind::kSprite ||
        ref->index >= movie.sprites.size())
      return nullptr;
    return &movie.sprites[ref->index];
  }

  // What `parent` places under the name `name`: the character, so the class the
  // movie bound to it can be found, and its timeline.
  struct ChildRef {
    const swf::Timeline* timeline = nullptr;
    u16 character = 0;
    // How opaque the placement left the clip. A Scaleform menu ships parts of
    // itself transparent and fades them in, so this is where a clip starts
    // rather than where it stays: code that sets `alpha` replaces it.
    f32 alpha = 1.0f;
  };
  ChildRef ChildOf(const swf::Timeline* parent, base::StringRef name) const {
    if (parent == nullptr)
      return ChildRef();
    for (const swf::Place& place : swf::DisplayListAt(*parent, 0)) {
      if (place.has_character && place.name == name) {
        return ChildRef{SpriteOf(place.character_id), place.character_id,
                        place.has_color_transform ? place.color_transform.mul_a : 1.0f};
      }
    }
    return ChildRef();
  }

  void BindAbcInto(ugui::UIContext& ui, const swf::As3Value& object, ugui::wid widget,
                   const ChildRef& placed, u32 depth) {
    if (depth > 16 || !object.is_object() || !widget.valid())
      return;
    ugui::WidgetRegistry& world = ui.world();
    // The class the movie bound to the character placed here. A nested panel is
    // a timeline class of its own, and its frame-1 script is what says how it
    // opens: every one of Fallout 4's ships hidden that way.
    if (const base::String* klass = movie.exports.find(placed.character))
      avm2->RunOpeningFrame(*klass, object);
    As3Bound entry;
    entry.object = object.object();
    entry.widget = widget;
    entry.timeline = placed.timeline;
    entry.placed_alpha = placed.alpha;
    const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(widget);
    entry.is_text = node && node->kind == ugui::WidgetKind::kText;
    as3_bound.push_back(entry);

    for (const base::String& key : avm2->Get(object.object()).order) {
      if (key.empty() || key[0] == '_')
        continue;
      const swf::As3Value child = avm2->GetProperty(object, key);
      if (!child.is_object())
        continue;
      base::Vector<ugui::wid> widgets;
      ChildrenNamed(world, widget, key, widgets);
      for (ugui::wid found : widgets)
        BindAbcInto(ui, child, found, ChildOf(placed.timeline, key), depth + 1);
    }
  }

  // The frame a bound object is on, zero-based. gotoAndStop records whatever it
  // was given: a number is the frame, one-based as the language counts them,
  // and a string is a label only the clip's own timeline can resolve.
  i32 CurrentFrame(const As3Bound& entry) {
    const swf::As3Value frame =
        avm2->GetProperty(swf::As3Value::Obj(entry.object), "currentFrame");
    if (frame.is_undefined())
      return 0;
    if (frame.is_string() && entry.timeline != nullptr) {
      for (mem_size f = 0; f < entry.timeline->frames.size(); ++f)
        if (entry.timeline->frames[f].label == frame.string())
          return static_cast<i32>(f);
      return 0;
    }
    const f64 number = avm2->ToNumber(frame);
    if (number != number || number < 1)
      return 0;
    return static_cast<i32>(number) - 1;
  }

  // How visible a clip's own timeline leaves it on a frame. A fade is authored
  // as a colour transform on what the clip places, not on the clip, so the most
  // opaque thing it puts on that frame is how opaque the clip is.
  f32 FrameAlpha(const swf::Timeline& timeline, u32 frame) {
    f32 alpha = 0.0f;
    bool any = false;
    for (const swf::Place& place : swf::DisplayListAt(timeline, frame)) {
      any = true;
      const f32 own = place.has_color_transform ? place.color_transform.mul_a : 1.0f;
      if (own > alpha)
        alpha = own;
    }
    return any ? alpha : 1.0f;
  }

  void HideOtherStates(ugui::WidgetRegistry& world, ugui::wid widget, ugui::wid active) {
    const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(widget);
    if (!h)
      return;
    for (ugui::wid child : h->children) {
      if (IsStateGroup(world, child))
        SetOpacity(world, child, Same(child, active) ? 1.0f : 0.0f);
    }
  }

  // A list's own entries onto the rows the translation stamped for it. An AS3
  // list builds its rows at runtime out of the symbol its bytecode names, so
  // the export stamps that many `Entry<n>` clips (see StampListRows) and this
  // is the other half: what the code put in `entryList` goes into them, and the
  // rows past the end are not drawn.
  void FillRows(ugui::WidgetRegistry& world, const swf::As3Value& object,
                ugui::wid widget) {
    const swf::As3Value list = avm2->GetProperty(object, "entryList");
    if (!list.is_object())
      return;
    const i32 count = static_cast<i32>(avm2->ToNumber(avm2->GetProperty(list, "length")));
    if (count <= 0)
      return;
    for (i32 row = 0;; ++row) {
      base::Vector<ugui::wid> widgets;
      ChildrenNamed(world, widget, base::Format("Entry{}", row), widgets);
      if (widgets.empty())
        break;
      if (row >= count) {
        SetOpacity(world, widgets[0], 0.0f);
        continue;
      }
      SetOpacity(world, widgets[0], 1.0f);
      const swf::As3Value item = avm2->GetProperty(list, base::Format("{}", row));
      const swf::As3Value text = avm2->GetProperty(item, "text");
      if (text.is_undefined())
        continue;
      const base::String label = Localise(avm2->ToString(text));
      WriteRow(world, widgets[0], label);
    }
  }

  // The one text widget a row holds. A stamped row is a whole clip - border,
  // spinner, icons - and only its field carries the entry's words.
  bool WriteRow(ugui::WidgetRegistry& world, ugui::wid widget, const base::String& text) {
    const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(widget);
    if (node && node->kind == ugui::WidgetKind::kText) {
      Write(world, widget, text);
      return true;
    }
    const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(widget);
    if (!h)
      return false;
    for (ugui::wid child : h->children)
      if (WriteRow(world, child, text))
        return true;
    return false;
  }

  // What the AS3 objects say about themselves, onto the widgets. The names
  // differ from AS2's: `visible` rather than `_visible`, and `alpha` runs 0..1.
  void SyncAbc(ugui::UIContext& ui) {
    ugui::WidgetRegistry& world = ui.world();
    for (const As3Bound& entry : as3_bound) {
      if (!avm2->Valid(entry.object) || !entry.widget.valid())
        continue;
      const swf::As3Value object = swf::As3Value::Obj(entry.object);
      const swf::As3Value visible = avm2->GetProperty(object, "visible");
      // Where the placement left the clip, unless its own code has set an alpha
      // since. The two are the same property in the player: the authored value
      // is what `alpha` starts at, not a second factor on top of it.
      f32 opacity = entry.placed_alpha;
      const swf::As3Value alpha = avm2->GetProperty(object, "alpha");
      if (!alpha.is_undefined()) {
        const f32 own = static_cast<f32>(avm2->ToNumber(alpha));
        if (own >= 0 && own <= 1)
          opacity = own;
      }
      if (!visible.is_undefined() && !avm2->ToBool(visible))
        opacity = 0.0f;
      const i32 at = CurrentFrame(entry);
      SetOpacity(world, entry.widget, opacity);
      // The rest of a timeline's states, the way the AS2 side does them: only
      // the group for the frame the clip is on is drawn.
      const ugui::wid active = ActiveState(world, entry.widget, at);
      if (active.valid())
        HideOtherStates(world, entry.widget, active);
      if (bool(VanillaReport)) {
        const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(entry.widget);
        RX_INFO("  vanilla as3: {} frame {} timeline {} opacity {}",
                node ? node->name.c_str() : "?", at,
                entry.timeline ? "yes" : "no", opacity);
      }
      if (entry.is_text) {
        const swf::As3Value text = avm2->GetProperty(object, "text");
        if (!text.is_undefined())
          Write(world, entry.widget, Localise(avm2->ToString(text)));
      }
      FillRows(world, object, entry.widget);
    }
  }

  // Runs an AS3 movie's classes the way the player does, and keeps whatever
  // ended up in the longest list.
  void RunAbc(swf::Avm2::ExternalHandler answerer, void* user) {
    avm2 = base::MakeUnique<swf::Avm2>();
    avm2->set_external_handler(answerer, user);
    for (const swf::AbcFile& file : abc)
      avm2->AddAbc(file);
    // Every class, then the message the game opens a screen with. An AS3 menu
    // takes that as a plain method rather than through a delegate.
    //
    // InitList's arguments are what the build offers, positionally. Of the ten,
    // three change what Fallout 4's main menu shows and the rest change
    // nothing: argument 0 adds QUIT, argument 1 adds ADD-ONS, argument 7 adds
    // the PlayStation save transfer. So: a PC build that can be quit and has no
    // add-ons wired.
    base::Vector<swf::As3Value> flags;
    for (int i = 0; i < 10; ++i)
      flags.push_back(swf::As3Value::Bool(i == 0));
    for (const swf::AbcFile& file : abc) {
      for (const swf::AbcClass& klass : file.classes) {
        if (klass.name.empty() || klass.is_interface)
          continue;
        const swf::As3Value instance = avm2->Construct(klass.name);
        if (!instance.is_object())
          continue;
        as3_instances[klass.name] = instance.object();
        // What the game hands a screen to talk back through, before it opens:
        // the screen asks its questions while it is coming up.
        avm2->SetProperty(instance, "BGSCodeObj",
                          swf::As3Value::Obj(avm2->NewCodeObject()));
        avm2->Invoke(instance, "SetPlatform", flags);
        avm2->Invoke(instance, "InitMenu", flags);
        avm2->Invoke(instance, "InitList", flags);
        base::Vector<swf::As3Value> version;
        version.push_back(swf::As3Value::Str("recreation"));
        version.push_back(swf::As3Value::Number(0));
        avm2->Invoke(instance, "SetVersionText", version);
        // The state the game opens this screen in. Fallout 4's main menu waits
        // on a splash, and it is the game that puts it there and the game that
        // takes it off again; the movie makes neither decision for itself.
        avm2->Invoke(instance, "SetToSplashScreen", base::Vector<swf::As3Value>());
        // ... and that the player may skip it, which is what puts the prompt up.
        base::Vector<swf::As3Value> skip;
        skip.push_back(swf::As3Value::Bool(true));
        skip.push_back(swf::As3Value::Bool(true));
        avm2->Invoke(instance, "SetAllowSkip", skip);
      }
    }
    for (u32 i = 1; i < avm2->object_count(); ++i) {
      const swf::As3Value list = avm2->GetProperty(swf::As3Value::Obj(i), "entryList");
      if (!list.is_object())
        continue;
      const i32 n = static_cast<i32>(avm2->ToNumber(avm2->GetProperty(list, "length")));
      if (n <= static_cast<i32>(entries.size()))
        continue;
      base::Vector<base::String> found;
      for (i32 e = 0; e < n; ++e) {
        const swf::As3Value entry = avm2->GetProperty(list, base::Format("{}", e));
        found.push_back(avm2->ToString(avm2->GetProperty(entry, "text")));
      }
      entries = base::move(found);
    }
  }

  struct Bound {
    u32 clip = 0;      // interpreter object
    ugui::wid widget;  // the translated widget it drives
    bool is_text = false;
    // Where the translation drew this widget. A script that moves its clip is
    // moved by the same amount from here, rather than to the clip's own
    // coordinates: the two origins differ by the character's own bounds.
    f32 left = 0;
    f32 top = 0;
    base::Vector<u32> children;  // indices into `bound`, in tree order
  };
  base::Vector<Bound> bound;
  // Where the translation put each widget, remembered the first time it is
  // bound. Re-reading it on a later bind would read a position this class had
  // already moved, and the offset would accumulate every frame until the
  // widget walked off the screen.
  base::UnorderedMap<u32, base::Pair<f32, f32>> origins;
  const base::UnorderedMap<base::String, base::String>* strings = nullptr;
  ugui::wid root;
  bool ready = false;
  // What the tree looked like when `bound` was built. A menu attaches clips as
  // it runs (a list makes its own rows), so the binding has to be redone when
  // the count moves rather than only at load.
  u32 bound_clips = 0;

  void Rebind(ugui::UIContext& ui) {
    bound.clear();
    Bind(ui, stage->root(), root, 0);
    bound_clips = stage->clip_count();
  }

  bool Same(ugui::wid a, ugui::wid b) const { return a.index == b.index; }

  // Walks the two trees together. They come from the same movie, so a clip's
  // instance name is the widget's name at the same place in the hierarchy.
  u32 Bind(ugui::UIContext& ui, const swf::AsValue& clip, ugui::wid widget, u32 depth) {
    if (depth > 24 || !clip.is_object() || !widget.valid())
      return kNoBinding;
    ugui::WidgetRegistry& world = ui.world();
    const u32 index = static_cast<u32>(bound.size());
    {
      Bound entry;
      entry.clip = clip.object();
      entry.widget = widget;
      const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(widget);
      entry.is_text = node && node->kind == ugui::WidgetKind::kText;
      if (const base::Pair<f32, f32>* known = origins.find(widget.index)) {
        entry.left = known->first;
        entry.top = known->second;
      } else if (const ugui::StyleC* sc = world.Get<ugui::StyleC>(widget)) {
        entry.left = sc->style.left_offset.value;
        entry.top = sc->style.top.value;
        origins[widget.index] = base::Pair<f32, f32>{entry.left, entry.top};
      }
      bound.push_back(base::move(entry));
    }

    // Only the clip's own members can be children; walking the prototype would
    // follow the class up into methods.
    const base::Vector<base::String> keys = vm.Get(clip.object()).order;
    for (const base::String& key : keys) {
      if (key.empty() || key[0] == '_')
        continue;  // _parent, _name and the rest are not children
      const swf::AsValue child = vm.GetMember(clip, key);
      if (!child.is_object() || !vm.Valid(child.object()))
        continue;
      const swf::AsObject& object = vm.Get(child.object());
      if (!object.is_movie_clip && object.props.find(base::String("text")) == nullptr)
        continue;
      base::Vector<ugui::wid> child_widgets;
      ChildrenNamed(world, widget, key, child_widgets);
      for (ugui::wid child_widget : child_widgets) {
        const u32 child_index = Bind(ui, child, child_widget, depth + 1);
        if (child_index != kNoBinding)
          bound[index].children.push_back(child_index);
      }
    }
    return index;
  }

  // "$KEY" resolved against the interface's own table, the way Scaleform's
  // translation layer did on the way to the screen. Left as-is when the table
  // has no such key, which is what the game shows too.
  // The text the translation baked into the markup. A movie ships its labels as
  // the game's own string keys ("$PIPBOY"), because the player looks them up
  // when it draws the field rather than when the field is authored.
  void LocaliseTree(ugui::WidgetRegistry& world, ugui::wid widget, u32 depth) {
    if (!widget.valid() || depth > 32)
      return;
    if (ugui::TextContent* content = world.Get<ugui::TextContent>(widget)) {
      const base::String text(content->text.c_str());
      if (!text.empty() && text[0] == '$') {
        const base::String localised = Localise(text);
        if (localised != text)
          Write(world, widget, localised);
      }
    }
    if (const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(widget)) {
      for (ugui::wid child : h->children)
        LocaliseTree(world, child, depth + 1);
    }
  }

  base::String Localise(const base::String& text) const {
    if (strings == nullptr || text.empty() || text[0] != '$')
      return text;
    const base::String* found = strings->find(text);
    return found ? *found : text;
  }

  // Copies what the script changed onto the widgets. Done once a frame rather
  // than written through on every property set: a menu touches these constantly
  // while it settles, and the screen only has to agree at the end of it.
  void Sync(ugui::UIContext& ui) {
    if (!bound.empty())
      SyncNode(ui, 0);
  }

  // One clip and everything the translation put under it. Only the clip's own
  // contribution goes on its widget: ugui inherits opacity, so a menu that
  // hides a whole page by clearing _visible takes the page's widgets with it.
  void SyncNode(ugui::UIContext& ui, u32 index) {
    const Bound& entry = bound[index];
    if (!vm.Valid(entry.clip) || !entry.widget.valid())
      return;
    const swf::AsValue clip = swf::AsValue::Obj(entry.clip);

    const swf::Stage::Placement placed = stage->PlacedAt(clip);
    f32 opacity = placed.alpha;
    const swf::AsValue visible = vm.GetMember(clip, "_visible");
    if (!visible.is_undefined() && !vm.ToBool(visible))
      opacity = 0.0f;
    const swf::AsValue alpha = vm.GetMember(clip, "_alpha");
    if (!alpha.is_undefined()) {
      const f32 own = static_cast<f32>(vm.ToNumber(alpha)) * 0.01f;
      if (own >= 0 && own <= 1)
        opacity *= own;
    }

    // What the clip's timeline is showing right now. The translation carries
    // one frame of it, so anything from another frame has to be taken down.
    base::Vector<base::String> current;
    base::Vector<base::String> ever;
    stage->PlacedNames(clip, current, ever);
    // Only the group for the frame the clip is on is drawn; the rest are the
    // states it is not in, which the translation carries so that it can be.
    const ugui::wid active =
        ActiveState(ui.world(), entry.widget,
                    static_cast<i32>(vm.ToNumber(vm.GetMember(clip, "_currentframe"))) - 1);
    Move(ui.world(), entry,
         static_cast<f32>(vm.ToNumber(vm.GetMember(clip, "_x"))) - placed.x,
         static_cast<f32>(vm.ToNumber(vm.GetMember(clip, "_y"))) - placed.y);
    Paint(ui.world(), entry.widget, opacity, entry, current, ever, active);
    if (entry.is_text) {
      const swf::AsValue text = vm.GetMember(clip, "text");
      if (!text.is_undefined())
        Write(ui.world(), entry.widget, Localise(vm.ToString(text)));
    }
    for (u32 child : entry.children)
      SyncNode(ui, child);
  }

  // A clip its script has moved off where the frame put it. A list lays its
  // rows out this way, so without this they all sit on top of each other.
  void Move(ugui::WidgetRegistry& world, const Bound& entry, f32 dx, f32 dy) {
    ugui::StyleC* sc = world.Get<ugui::StyleC>(entry.widget);
    if (!sc)
      return;
    const f32 left = entry.left + (dx == dx ? dx : 0.0f);
    const f32 top = entry.top + (dy == dy ? dy : 0.0f);
    if (sc->style.left_offset.value == left && sc->style.top.value == top)
      return;
    ugui::Style style = sc->style;
    style.left_offset.value = left;
    style.top.value = top;
    ugui::SetStyle(world, entry.widget, style);
  }

  // A field the movie fills. Its authored colour is often fully transparent:
  // the designer leaves the placeholder invisible because the game is what puts
  // words there, and the transparency is the placeholder's, not the text's. So
  // writing to a field is also what makes it legible; how visible it ends up is
  // the clip's own _alpha, which Paint has already applied.
  void Write(ugui::WidgetRegistry& world, ugui::wid widget, const base::String& text) {
    ugui::StyleC* sc = world.Get<ugui::StyleC>(widget);
    if (sc && sc->style.text_color.a <= 0.0f) {
      ugui::Style style = sc->style;
      style.text_color.a = 1.0f;
      ugui::SetStyle(world, widget, style);
    }
    // Written through the registry this screen lives in. ugui::SetText resolves
    // the widget against whichever registry is Active(), which is not
    // necessarily this one, and a write that lands in the wrong registry is
    // silent: the label stays as the translation left it.
    world.GetOrAdd<ugui::TextContent>(widget).text = text.c_str();
    ugui::MarkDirty(world, widget);
  }

  void SetOpacity(ugui::WidgetRegistry& world, ugui::wid widget, f32 opacity) {
    ugui::StyleC* sc = world.Get<ugui::StyleC>(widget);
    if (!sc || sc->style.opacity == opacity)
      return;
    ugui::Style style = sc->style;
    style.opacity = opacity;
    ugui::SetStyle(world, widget, style);
  }

  // The clip's own opacity, then whatever of its subtree this frame is not
  // showing. Only widgets the timeline has something to say about are written:
  // the rest keep the alpha the placement authored, and inherit the clip's.
  void Paint(ugui::WidgetRegistry& world, ugui::wid widget, f32 opacity,
             const Bound& owner, const base::Vector<base::String>& current,
             const base::Vector<base::String>& ever, ugui::wid active) {
    if (!widget.valid())
      return;
    SetOpacity(world, widget, opacity);
    const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(widget);
    if (!h)
      return;
    for (ugui::wid child : h->children) {
      bool owned = false;
      for (u32 index : owner.children)
        owned = owned || Same(bound[index].widget, child);
      if (owned)
        continue;  // that clip resolves its own state
      // Only the group for the frame the clip is on is drawn, and taking a
      // group down takes everything under it, so the walk stops there.
      if (IsStateGroup(world, child)) {
        SetOpacity(world, child, Same(child, active) ? 1.0f : 0.0f);
        continue;
      }
      Paint(world, child, OnStage(world, child, current, ever) ? 1.0f : 0.0f, owner,
            current, ever, active);
    }
  }

  // Whether a widget the owning clip did not bind is part of what that clip is
  // showing now. Only names the timeline itself places are judged: everything
  // else is art or a container the translation added, and follows its holder.
  bool OnStage(ugui::WidgetRegistry& world, ugui::wid widget,
               const base::Vector<base::String>& current,
               const base::Vector<base::String>& ever) const {
    const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(widget);
    if (!node)
      return true;
    const base::StringRef name(node->name.c_str());
    for (const base::String& shown : current)
      if (NameMatches(shown, name))
        return true;
    for (const base::String& known : ever)
      if (NameMatches(known, name))
        return false;
    return true;
  }
};

VanillaRuntime::VanillaRuntime() = default;
VanillaRuntime::~VanillaRuntime() = default;
VanillaRuntime::VanillaRuntime(VanillaRuntime&&) noexcept = default;
VanillaRuntime& VanillaRuntime::operator=(VanillaRuntime&&) noexcept = default;

bool VanillaRuntime::Enabled() {
  return bool(VanillaVm);
}

base::Vector<base::String> VanillaRuntime::ListEntries() const {
  base::Vector<base::String> out;
  if (!impl_)
    return out;
  for (const base::String& entry : impl_->entries)
    out.push_back(entry);
  return out;
}

void VanillaRuntime::SetStrings(
    const base::UnorderedMap<base::String, base::String>* strings) {
  strings_ = strings;
  if (impl_)
    impl_->strings = strings;
}

bool VanillaRuntime::loaded() const {
  return impl_ && impl_->ready;
}
u32 VanillaRuntime::bound_count() const {
  return impl_ ? static_cast<u32>(impl_->bound.size()) : 0;
}
u32 VanillaRuntime::clip_count() const {
  return impl_ && impl_->stage ? impl_->stage->clip_count() : 0;
}

bool VanillaRuntime::Load(ugui::UIContext& ui, base::StringRef dir, base::StringRef screen,
                          base::StringRef root) {
  impl_ = base::MakeUnique<Impl>();
  impl_->strings = strings_;
  const fs::path path =
      fs::path(base::String(dir).c_str()) / (base::String(screen) + ".swf").c_str();
  impl_->bytes = ReadFile(path);
  if (impl_->bytes.empty()) {
    RX_WARN("vanilla vm: {} not found (re-run swfdump --ugui-all)", path.string());
    impl_.Reset();
    return false;
  }

  auto file = swf::OpenSwf(ByteSpan{impl_->bytes.data(), impl_->bytes.size()});
  if (!file.has_value()) {
    impl_.Reset();
    return false;
  }
  impl_->file = base::move(file.value());
  auto movie = swf::LoadMovie(impl_->file);
  if (!movie.has_value()) {
    impl_.Reset();
    return false;
  }
  impl_->movie = base::move(movie.value());

  if (!impl_->movie.abc_blocks.empty()) {
    for (const ByteSpan& block : impl_->movie.abc_blocks) {
      swf::AbcFile file;
      if (swf::ParseAbc(block, file))
        impl_->abc.push_back(base::move(file));
    }
    impl_->RunAbc(as3_answerer_, as3_answer_user_);
    const ugui::wid as3_root = ui.FindWidget(base::String(root).c_str());
    if (as3_root.valid()) {
      impl_->root = as3_root;
      impl_->LocaliseTree(ui.world(), as3_root, 0);
      impl_->BindAbc(ui, as3_root);
      if (const char* send = VanillaSend) {
        for (const auto& entry : impl_->as3_instances) {
          impl_->avm2->Invoke(swf::As3Value::Obj(entry.value), send,
                              base::Vector<swf::As3Value>());
        }
      }
      impl_->SyncAbc(ui);
    }
    impl_->ready = true;
    RX_INFO("vanilla vm: {} is actionscript 3, {} entr{} from its own code, "
            "{} object(s) bound to widgets",
            screen, impl_->entries.size(), impl_->entries.size() == 1 ? "y" : "ies",
            impl_->as3_bound.size());
    return true;
  }

  impl_->bridge = base::MakeUnique<swf::GameBridge>(impl_->vm);
  impl_->bridge->set_answerer(answerer_, answer_user_);
  impl_->stage = base::MakeUnique<swf::Stage>(impl_->vm, impl_->movie);
  impl_->stage->set_authored_state(bool(VanillaAuthored));
  impl_->stage->Run();
  // What the game does once a menu's code object is up. Most of a menu's
  // callbacks are registered in there, so without it the screen is listening
  // for almost nothing the host might send.
  impl_->bridge->Open();

  const ugui::wid root_widget = ui.FindWidget(base::String(root).c_str());
  if (!root_widget.valid()) {
    RX_WARN("vanilla vm: no widget named {} to bind {} to", root, screen);
    impl_.Reset();
    return false;
  }
  impl_->root = root_widget;
  impl_->LocaliseTree(ui.world(), root_widget, 0);
  impl_->Rebind(ui);
  impl_->Sync(ui);
  impl_->ready = true;
  RX_INFO("vanilla vm: {} ran {} clip(s), {} bound to widgets, listening for {}",
          screen, impl_->stage->clip_count(), impl_->bound.size(),
          impl_->bridge->Callbacks().size());
  return true;
}

void VanillaRuntime::SetAs3Answerer(swf::Avm2::ExternalHandler handler, void* user) {
  as3_answerer_ = handler;
  as3_answer_user_ = user;
  if (impl_ && impl_->avm2)
    impl_->avm2->set_external_handler(handler, user);
}

base::Vector<base::String> VanillaRuntime::As3Calls() const {
  base::Vector<base::String> out;
  if (!impl_ || !impl_->avm2)
    return out;
  for (const base::String& call : impl_->avm2->external_calls())
    out.push_back(call);
  return out;
}

void VanillaRuntime::SetAnswerer(swf::GameBridge::Answerer answerer, void* user) {
  answerer_ = answerer;
  answer_user_ = user;
  if (impl_ && impl_->bridge)
    impl_->bridge->set_answerer(answerer, user);
}

bool VanillaRuntime::CallAs3(ugui::UIContext& ui, base::StringRef name,
                             const base::Vector<swf::As3Value>& args) {
  if (!impl_ || !impl_->ready || !impl_->avm2)
    return false;
  bool any = false;
  // Every class the movie placed. The game addresses a screen as one object,
  // but which class that is differs per movie, and a name only one of them has
  // reaches only that one.
  for (const auto& entry : impl_->as3_instances) {
    if (impl_->avm2->Invoke(swf::As3Value::Obj(entry.value), name, args))
      any = true;
  }
  if (any)
    impl_->SyncAbc(ui);
  return any;
}

bool VanillaRuntime::CallRoot(ugui::UIContext& ui, base::StringRef name,
                              const base::Vector<swf::AsValue>& args) {
  if (!impl_ || !impl_->ready || !impl_->stage)
    return false;
  if (!impl_->stage->Dispatch(impl_->stage->root(), name, args))
    return false;
  impl_->Rebind(ui);
  impl_->Sync(ui);
  return true;
}

bool VanillaRuntime::Navigate(ugui::UIContext& ui, base::StringRef navigation) {
  if (!impl_ || !impl_->ready || !impl_->stage)
    return false;
  swf::Vm& vm = impl_->vm;
  const swf::AsValue details = swf::AsValue::Obj(vm.NewObject());
  vm.SetMember(details, "navEquivalent", swf::AsValue::Str(navigation));
  vm.SetMember(details, "value", swf::AsValue::Str("keyDown"));
  vm.SetMember(details, "code", swf::AsValue::Number(0));
  vm.SetMember(details, "controllerIdx", swf::AsValue::Number(0));
  const swf::AsValue path = swf::AsValue::Obj(vm.NewArray());
  vm.SetMember(path, "length", swf::AsValue::Number(0));

  // The game routes a key through its focus manager to the component that has
  // focus. There is no focus manager here, so the key goes to the lists that
  // have something in them, which is what the player is looking at.
  bool handled = false;
  for (const Impl::Bound& entry : impl_->bound) {
    if (!vm.Valid(entry.clip))
      continue;
    const swf::AsValue clip = swf::AsValue::Obj(entry.clip);
    const swf::AsValue entries = vm.GetMember(clip, "entryList");
    if (!entries.is_object() || vm.ToNumber(vm.GetMember(entries, "length")) <= 0)
      continue;
    if (!vm.ToBool(vm.GetMember(clip, "_visible")))
      continue;
    base::Vector<swf::AsValue> args;
    args.push_back(details);
    args.push_back(path);
    if (impl_->stage->Dispatch(clip, "handleInput", args))
      handled = true;
  }
  if (handled) {
    impl_->Rebind(ui);
    impl_->Sync(ui);
  }
  return handled;
}

bool VanillaRuntime::Send(ugui::UIContext& ui, base::StringRef name,
                          const base::Vector<swf::AsValue>& args) {
  if (!impl_ || !impl_->ready || !impl_->bridge)
    return false;
  if (!impl_->bridge->Invoke(name, args))
    return false;
  impl_->Rebind(ui);
  impl_->Sync(ui);
  return true;
}

base::Vector<swf::GameBridge::Call> VanillaRuntime::TakePending() {
  base::Vector<swf::GameBridge::Call> out;
  if (!impl_ || !impl_->ready || !impl_->bridge)
    return out;
  for (const swf::GameBridge::Call& call : impl_->bridge->pending()) {
    swf::GameBridge::Call copy;
    copy.name = call.name;
    copy.id = call.id;
    for (const swf::AsValue& arg : call.args)
      copy.args.push_back(arg);
    out.push_back(base::move(copy));
  }
  impl_->bridge->ClearPending();
  return out;
}

bool VanillaRuntime::Respond(ugui::UIContext& ui, u32 id,
                             const base::Vector<swf::AsValue>& args) {
  if (!impl_ || !impl_->ready || !impl_->bridge || !impl_->bridge->Respond(id, args))
    return false;
  impl_->Rebind(ui);
  impl_->Sync(ui);
  return true;
}

swf::Vm* VanillaRuntime::vm() {
  return impl_ && impl_->ready && impl_->stage ? &impl_->vm : nullptr;
}

void VanillaRuntime::Tick(ugui::UIContext& ui, f32 delta_seconds) {
  if (!impl_ || !impl_->ready)
    return;
  // An ActionScript 3 screen has no timeline to step here - the class code that
  // built it ran at load - but what the host says to it afterwards changes it,
  // and the widgets only agree once that is written through.
  if (!impl_->stage) {
    if (impl_->avm2)
      impl_->SyncAbc(ui);
    return;
  }
  impl_->stage->Tick(static_cast<f64>(delta_seconds) * 1000.0);
  if (impl_->stage->clip_count() != impl_->bound_clips)
    impl_->Rebind(ui);
  impl_->Sync(ui);
}

bool VanillaRuntime::Click(ugui::UIContext& ui, u32 widget) {
  if (!impl_ || !impl_->ready || !impl_->stage)
    return false;
  for (const Impl::Bound& entry : impl_->bound) {
    if (entry.widget.index != widget)
      continue;
    const swf::AsValue clip = swf::AsValue::Obj(entry.clip);
    bool handled = impl_->stage->Dispatch(clip, "onPress");
    handled = impl_->stage->Dispatch(clip, "onRelease") || handled;
    if (handled) {
      if (impl_->stage->clip_count() != impl_->bound_clips)
        impl_->Rebind(ui);
      impl_->Sync(ui);
    }
    return handled;
  }
  return false;
}

}  // namespace rx::ui

#else  // RECREATION_HAS_UGUI

namespace rx::ui {

struct VanillaRuntime::Impl {};
VanillaRuntime::VanillaRuntime() = default;
VanillaRuntime::~VanillaRuntime() = default;
VanillaRuntime::VanillaRuntime(VanillaRuntime&&) noexcept = default;
VanillaRuntime& VanillaRuntime::operator=(VanillaRuntime&&) noexcept = default;
bool VanillaRuntime::Enabled() {
  return false;
}
base::Vector<base::String> VanillaRuntime::ListEntries() const {
  base::Vector<base::String> out;
  if (!impl_)
    return out;
  for (const base::String& entry : impl_->entries)
    out.push_back(entry);
  return out;
}

void VanillaRuntime::SetStrings(
    const base::UnorderedMap<base::String, base::String>* strings) {
  strings_ = strings;
}
bool VanillaRuntime::loaded() const {
  return false;
}
u32 VanillaRuntime::bound_count() const {
  return 0;
}
u32 VanillaRuntime::clip_count() const {
  return 0;
}
bool VanillaRuntime::Load(ugui::UIContext&, base::StringRef, base::StringRef,
                          base::StringRef) {
  return false;
}
void VanillaRuntime::Tick(ugui::UIContext&, f32) {}
bool VanillaRuntime::Click(ugui::UIContext&, u32) {
  return false;
}
bool VanillaRuntime::Navigate(ugui::UIContext&, base::StringRef) {
  return false;
}
bool VanillaRuntime::CallRoot(ugui::UIContext&, base::StringRef,
                              const base::Vector<swf::AsValue>&) {
  return false;
}
bool VanillaRuntime::CallAs3(ugui::UIContext&, base::StringRef,
                             const base::Vector<swf::As3Value>&) {
  return false;
}
bool VanillaRuntime::Send(ugui::UIContext&, base::StringRef,
                          const base::Vector<swf::AsValue>&) {
  return false;
}
base::Vector<swf::GameBridge::Call> VanillaRuntime::TakePending() {
  return base::Vector<swf::GameBridge::Call>();
}
bool VanillaRuntime::Respond(ugui::UIContext&, u32, const base::Vector<swf::AsValue>&) {
  return false;
}
swf::Vm* VanillaRuntime::vm() {
  return nullptr;
}
void VanillaRuntime::SetAnswerer(swf::GameBridge::Answerer, void*) {}

}  // namespace rx::ui

#endif  // RECREATION_HAS_UGUI
