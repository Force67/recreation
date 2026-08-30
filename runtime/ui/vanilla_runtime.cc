#include "runtime/ui/vanilla_runtime.h"

#include <base/memory/move.h>
#include <base/option.h>

#if defined(RECREATION_HAS_UGUI)

#include <ugui/ultragui.h>
#include <ugui/widgets/text.h>
#include <ugui/widgets/widget.h>

#include <filesystem>
#include <fstream>

#include "components/swf/movie.h"
#include "components/swf/stage.h"
#include "components/swf/swf.h"
#include "components/swf/vm.h"
#include "core/log.h"

namespace rx::ui {
namespace {

namespace fs = std::filesystem;

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
  base::UniquePointer<swf::Stage> stage;

  struct Bound {
    u32 clip = 0;      // interpreter object
    ugui::wid widget;  // the translated widget it drives
    bool is_text = false;
  };
  base::Vector<Bound> bound;
  bool ready = false;

  // Walks the two trees together. They come from the same movie, so a clip's
  // instance name is the widget's name at the same place in the hierarchy.
  void Bind(ugui::UIContext& ui, const swf::AsValue& clip, ugui::wid widget, u32 depth) {
    if (depth > 24 || !clip.is_object() || !widget.valid())
      return;
    ugui::WidgetRegistry& world = ui.world();
    Bound entry;
    entry.clip = clip.object();
    entry.widget = widget;
    const ugui::WidgetNode* node = world.Get<ugui::WidgetNode>(widget);
    entry.is_text = node && node->kind == ugui::WidgetKind::kText;
    bound.push_back(entry);

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
      if (child_widget.valid())
        Bind(ui, child, child_widget, depth + 1);
    }
  }

  // Copies what the script changed onto the widgets. Done once a frame rather
  // than written through on every property set: a menu touches these constantly
  // while it settles, and the screen only has to agree at the end of it.
  void Sync(ugui::UIContext& ui) {
    ugui::WidgetRegistry& world = ui.world();
    for (const Bound& entry : bound) {
      if (!vm.Valid(entry.clip) || !entry.widget.valid())
        continue;
      const swf::AsValue clip = swf::AsValue::Obj(entry.clip);
      ugui::StyleC* sc = world.Get<ugui::StyleC>(entry.widget);
      if (!sc)
        continue;
      ugui::Style style = sc->style;
      bool changed = false;

      const swf::AsValue visible = vm.GetMember(clip, "_visible");
      if (!visible.is_undefined()) {
        const ugui::Visibility want =
            vm.ToBool(visible) ? ugui::Visibility::kVisible : ugui::Visibility::kCollapsed;
        if (style.visibility != want) {
          style.visibility = want;
          changed = true;
        }
      }
      const swf::AsValue alpha = vm.GetMember(clip, "_alpha");
      if (!alpha.is_undefined()) {
        const f32 want = static_cast<f32>(vm.ToNumber(alpha)) * 0.01f;
        if (want >= 0 && want <= 1 && style.opacity != want) {
          style.opacity = want;
          changed = true;
        }
      }
      if (changed)
        ugui::SetStyle(world, entry.widget, style);

      if (entry.is_text) {
        const swf::AsValue text = vm.GetMember(clip, "text");
        if (!text.is_undefined())
          ugui::SetText(entry.widget, vm.ToString(text).c_str());
      }
    }
  }
};

VanillaRuntime::VanillaRuntime() = default;
VanillaRuntime::~VanillaRuntime() = default;
VanillaRuntime::VanillaRuntime(VanillaRuntime&&) noexcept = default;
VanillaRuntime& VanillaRuntime::operator=(VanillaRuntime&&) noexcept = default;

bool VanillaRuntime::Enabled() {
  return bool(VanillaVm);
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

  impl_->stage = base::MakeUnique<swf::Stage>(impl_->vm, impl_->movie);
  impl_->stage->Run();

  const ugui::wid root_widget = ui.FindWidget(base::String(root).c_str());
  if (!root_widget.valid()) {
    RX_WARN("vanilla vm: no widget named {} to bind {} to", root, screen);
    impl_.Reset();
    return false;
  }
  impl_->Bind(ui, impl_->stage->root(), root_widget, 0);
  impl_->Sync(ui);
  impl_->ready = true;
  RX_INFO("vanilla vm: {} ran {} clip(s), {} bound to widgets", screen,
          impl_->stage->clip_count(), impl_->bound.size());
  return true;
}

void VanillaRuntime::Tick(ugui::UIContext& ui, f32 delta_seconds) {
  if (!impl_ || !impl_->ready)
    return;
  impl_->stage->Tick(static_cast<f64>(delta_seconds) * 1000.0);
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
    if (handled)
      impl_->Sync(ui);
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

}  // namespace rx::ui

#endif  // RECREATION_HAS_UGUI
