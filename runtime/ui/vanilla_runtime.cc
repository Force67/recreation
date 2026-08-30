#include "runtime/ui/vanilla_runtime.h"

#include <base/memory/move.h>
#include <base/option.h>

#if defined(RECREATION_HAS_UGUI)

#include <ugui/ultragui.h>
#include <ugui/widgets/text.h>
#include <ugui/widgets/widget.h>

#include <filesystem>
#include <fstream>

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
base::Option<bool> VanillaVm{"ui.vanilla.vm", false, "RX_VANILLA_VM"};

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

ugui::wid ChildNamed(ugui::WidgetRegistry& world, ugui::wid parent, base::StringRef name) {
  const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(parent);
  if (!h)
    return ugui::kNullWidget;
  ugui::wid fallback = ugui::kNullWidget;
  for (ugui::wid child : h->children) {
    const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(child);
    if (!node)
      continue;
    const base::StringRef actual(node->name.c_str());
    if (name == actual)
      return child;  // an exact match always wins
    if (!fallback.valid() && NameMatches(name, actual))
      fallback = child;
  }
  return fallback;
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
      if (const ugui::StyleC* sc = world.Get<ugui::StyleC>(widget)) {
        entry.left = sc->style.left_offset.value;
        entry.top = sc->style.top.value;
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
      const ugui::wid child_widget = ChildNamed(world, widget, key);
      if (!child_widget.valid())
        continue;
      const u32 child_index = Bind(ui, child, child_widget, depth + 1);
      if (child_index != kNoBinding)
        bound[index].children.push_back(child_index);
    }
    return index;
  }

  // "$KEY" resolved against the interface's own table, the way Scaleform's
  // translation layer did on the way to the screen. Left as-is when the table
  // has no such key, which is what the game shows too.
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
      SyncNode(ui, 0, 1.0f);
  }

  // One clip and everything the translation put under it. `inherited` is the
  // opacity the clips above it resolved to: a menu hides a whole page by
  // clearing _visible on the page, and the widgets under it have to follow.
  void SyncNode(ugui::UIContext& ui, u32 index, f32 inherited) {
    const Bound& entry = bound[index];
    if (!vm.Valid(entry.clip) || !entry.widget.valid())
      return;
    const swf::AsValue clip = swf::AsValue::Obj(entry.clip);

    const swf::Stage::Placement placed = stage->PlacedAt(clip);
    f32 opacity = inherited * placed.alpha;
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
    Move(ui.world(), entry,
         static_cast<f32>(vm.ToNumber(vm.GetMember(clip, "_x"))) - placed.x,
         static_cast<f32>(vm.ToNumber(vm.GetMember(clip, "_y"))) - placed.y);
    Paint(ui.world(), entry.widget, opacity, entry, current, ever);
    if (entry.is_text) {
      const swf::AsValue text = vm.GetMember(clip, "text");
      if (!text.is_undefined())
        Write(ui.world(), entry.widget, Localise(vm.ToString(text)));
    }
    for (u32 child : entry.children)
      SyncNode(ui, child, opacity);
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
    ugui::SetText(widget, text.c_str());
  }

  // ugui does not inherit opacity, so a clip's has to be written onto every
  // widget under it. The walk stops wherever a child clip took over, since that
  // clip resolves its own and paints its own subtree.
  void Paint(ugui::WidgetRegistry& world, ugui::wid widget, f32 opacity,
             const Bound& owner, const base::Vector<base::String>& current,
             const base::Vector<base::String>& ever) {
    if (!widget.valid())
      return;
    if (ugui::StyleC* sc = world.Get<ugui::StyleC>(widget); sc && sc->style.opacity != opacity) {
      ugui::Style style = sc->style;
      style.opacity = opacity;
      ugui::SetStyle(world, widget, style);
    }
    const ugui::Hierarchy* h = world.Get<ugui::Hierarchy>(widget);
    if (!h)
      return;
    for (ugui::wid child : h->children) {
      bool owned = false;
      for (u32 index : owner.children)
        owned = owned || Same(bound[index].widget, child);
      if (owned)
        continue;  // that clip resolves its own state
      Paint(world, child, OnStage(world, child, current, ever) ? opacity : 0.0f, owner,
            current, ever);
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

  impl_->bridge = base::MakeUnique<swf::GameBridge>(impl_->vm);
  impl_->stage = base::MakeUnique<swf::Stage>(impl_->vm, impl_->movie);
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
  impl_->Rebind(ui);
  impl_->Sync(ui);
  impl_->ready = true;
  RX_INFO("vanilla vm: {} ran {} clip(s), {} bound to widgets, listening for {}",
          screen, impl_->stage->clip_count(), impl_->bound.size(),
          impl_->bridge->Callbacks().size());
  return true;
}

bool VanillaRuntime::Send(ugui::UIContext& ui, base::StringRef name,
                          const base::Vector<swf::AsValue>& args) {
  if (!impl_ || !impl_->ready)
    return false;
  if (!impl_->bridge->Invoke(name, args))
    return false;
  impl_->Rebind(ui);
  impl_->Sync(ui);
  return true;
}

base::Vector<base::String> VanillaRuntime::TakePending() {
  base::Vector<base::String> out;
  if (!impl_ || !impl_->ready)
    return out;
  for (const swf::GameBridge::Call& call : impl_->bridge->pending())
    out.push_back(call.name);
  impl_->bridge->ClearPending();
  return out;
}

void VanillaRuntime::Tick(ugui::UIContext& ui, f32 delta_seconds) {
  if (!impl_ || !impl_->ready)
    return;
  impl_->stage->Tick(static_cast<f64>(delta_seconds) * 1000.0);
  if (impl_->stage->clip_count() != impl_->bound_clips)
    impl_->Rebind(ui);
  impl_->Sync(ui);
}

bool VanillaRuntime::Click(ugui::UIContext& ui, u32 widget) {
  if (!impl_ || !impl_->ready)
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
bool VanillaRuntime::Send(ugui::UIContext&, base::StringRef,
                          const base::Vector<swf::AsValue>&) {
  return false;
}
base::Vector<base::String> VanillaRuntime::TakePending() {
  return base::Vector<base::String>();
}

}  // namespace rx::ui

#endif  // RECREATION_HAS_UGUI
