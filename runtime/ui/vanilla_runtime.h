#ifndef RECREATION_RUNTIME_UI_VANILLA_RUNTIME_H_
#define RECREATION_RUNTIME_UI_VANILLA_RUNTIME_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/swf/avm2.h"
#include "components/swf/bridge.h"
#include "components/swf/vm.h"
#include "core/types.h"

namespace ugui {
class UIContext;
}  // namespace ugui

namespace rx::ui {

// Runs a translated screen's own ActionScript against its widgets.
//
// The translation is static: swfdump turns a movie into markup, and for a while
// the host filled that in by hand, which meant reimplementing each menu's logic
// in C++. This is the other way round - load the movie beside the markup, run
// its code on the interpreter in components/swf, and let what the code does to
// its clips reach the widgets the same names were translated into.
//
// The two trees line up because they come from the same movie: a clip's
// instance name is the widget's name and the hierarchy matches, so binding is a
// walk of both at once. What the script then changes - visibility, position,
// text, which frame a state clip is on - is written through to the widget.
//
// This is how every one of these screens runs now (RX_VANILLA_VM=0 turns it
// off, which leaves the screen as the translation drew it). Skyrim's menus are
// ActionScript 2 and run on the AVM1 machine; Fallout 4's and Starfield's are
// ActionScript 3 and run on the AVM2 one beside it. The two differ in how the
// game talks to a screen - AS2 through GameDelegate, AS3 through a code object
// the game hands over - and in little else the host has to care about.
class VanillaRuntime {
 public:
  VanillaRuntime();
  ~VanillaRuntime();
  VanillaRuntime(VanillaRuntime&&) noexcept;
  VanillaRuntime& operator=(VanillaRuntime&&) noexcept;

  // Whether the host asked for this at all.
  static bool Enabled();

  // The entries a screen's own code put in its longest list, in order. This is
  // the ActionScript 3 path: Fallout 4's and Starfield's menus are AVM2, whose
  // list component is not executed here, so the rows stay the ones the
  // translation stamped and only what goes in them comes from the movie. Empty
  // for an AS2 screen, whose list fills its own widgets directly.
  base::Vector<base::String> ListEntries() const;

  // The interface's "$KEY" table (see LoadVanillaStrings). A menu writes the
  // keys, not the words: Scaleform resolved them on the way to the screen and
  // so does this. Must outlive the runtime; nothing is resolved without it.
  void SetStrings(const base::UnorderedMap<base::String, base::String>* strings);

  // Loads <dir>/<screen>.swf, runs its library code, builds its display list
  // and binds it to the widgets under `root`. False when the movie is missing
  // or carries no script worth running.
  bool Load(ugui::UIContext& ui, base::StringRef dir, base::StringRef screen,
            base::StringRef root);

  // One frame: fires the movie's timers and onEnterFrame, then writes whatever
  // changed back to the widgets.
  void Tick(ugui::UIContext& ui, f32 delta_seconds);

  // A click landed on `widget`: sends onPress to the clip bound to it, if any.
  // Returns whether the movie's own code handled it.
  bool Click(ugui::UIContext& ui, u32 widget);

  // Calls a function the movie's root defines. The game reaches a menu two
  // ways: through GameDelegate for anything it registered, and directly for
  // the handful a screen exposes on its own timeline (`SetPlatform` is the one
  // that matters - a list dims and re-lays itself for a controller until it is
  // told it is on a PC). False when the root has no such function.
  bool CallRoot(ugui::UIContext& ui, base::StringRef name,
                const base::Vector<swf::AsValue>& args);

  // The same for an ActionScript 3 screen, which the game addresses as plain
  // methods rather than through a delegate: `InitList`, `SetPlatform`,
  // `ReturnToMainState`. Sent to every class the movie placed, since which one
  // is the screen differs per movie. False when none of them has that method.
  bool CallAs3(ugui::UIContext& ui, base::StringRef name,
               const base::Vector<swf::As3Value>& args);

  // A navigation key or pad press, as gfx.ui.NavigationCode names it: "up",
  // "down", "left", "right", "enter", "escape". Reaches the components through
  // the movie's own `handleInput`, so it drives every screen's navigation
  // rather than one screen's. Returns whether anything took it.
  bool Navigate(ugui::UIContext& ui, base::StringRef navigation);

  // Sends the host's side of the conversation, e.g. "sendMenuProperties" with
  // what this build offers. False when the movie is not listening for `name`.
  // See components/swf/bridge.h for which direction is which.
  bool Send(ugui::UIContext& ui, base::StringRef name,
            const base::Vector<swf::AsValue>& args);

  // Installs what answers the screen's questions. It is called from inside the
  // movie's own call, which is the only time an answer can land: see
  // GameBridge::set_answerer. `user` is handed back untouched.
  void SetAnswerer(swf::GameBridge::Answerer answerer, void* user);

  // The same for an ActionScript 3 screen, which asks through the code object
  // the game hands it rather than through GameDelegate. Install before Load:
  // an AS3 screen asks its questions while it is opening.
  void SetAs3Answerer(swf::Avm2::ExternalHandler handler, void* user);

  // What an ActionScript 3 screen asked the host for, in order.
  base::Vector<base::String> As3Calls() const;

  // What the movie asked the host for and nothing answered, with the arguments
  // it passed. Drains the queue; answer any of them with Respond before the
  // next tick, since the movie drops its response slot as soon as the call it
  // is waiting on returns.
  base::Vector<swf::GameBridge::Call> TakePending();

  // Answers a call from TakePending. `id` is the call's own.
  bool Respond(ugui::UIContext& ui, u32 id, const base::Vector<swf::AsValue>& args);

  // The interpreter behind the screen, for a host that has to reach a value the
  // bridge cannot carry (a call that hands over one of its own text fields).
  swf::Vm* vm();

  bool loaded() const;
  // How many clips found a widget, for the log line that says whether the two
  // trees actually lined up.
  u32 bound_count() const;
  u32 clip_count() const;

 private:
  struct Impl;
  base::UniquePointer<Impl> impl_;
  const base::UnorderedMap<base::String, base::String>* strings_ = nullptr;
  swf::GameBridge::Answerer answerer_ = nullptr;
  void* answer_user_ = nullptr;
  swf::Avm2::ExternalHandler as3_answerer_ = nullptr;
  void* as3_answer_user_ = nullptr;
};

}  // namespace rx::ui

#endif  // RECREATION_RUNTIME_UI_VANILLA_RUNTIME_H_
