#include "components/swf/stage.h"

#include "components/swf/text.h"

#include <base/algorithm.h>
#include <base/memory/move.h>
#include <base/strings/format.h>

namespace rx::swf {
namespace {

// Nesting cap: a movie can place a sprite inside itself, and the tree is walked
// eagerly rather than on demand.
constexpr u32 kMaxDepth = 24;

// The display list as it stands on `frame`, applying every frame up to it in
// order: a later frame moves, replaces or clears what an earlier one placed.

base::String NameFor(const Place& place) {
  return place.name.empty() ? base::Format("instance{}", place.depth) : place.name;
}

// The clip a native was called on, as the stage's 1-based table index. Zero is
// "not a clip this stage built".
u32 HostIndex(Vm& vm, const AsValue& self) {
  if (!self.is_object() || !vm.Valid(self.object()))
    return 0;
  return static_cast<u32>(vm.Get(self.object()).host);
}

}  // namespace

Stage::Stage(Vm& vm, const Movie& movie) : vm_(vm), movie_(movie) {}

u32 Stage::StateIndexOf(const AsValue& clip) const {
  if (!clip.is_object() || !vm_.Valid(clip.object()))
    return 0;
  return static_cast<u32>(vm_.Get(clip.object()).host);
}

// Where a placed character sits and how big it is, in pixels. A menu lays
// itself out from these: a scrolling list measures its border to decide how
// many rows fit and reads each row's height to step down the column, so a clip
// without them computes NaN and hides every row it was about to show.
void Stage::PlaceGeometry(const AsValue& self, const Place& place) {
  const Rect bounds = Transform(place.matrix, CharacterBounds(movie_, place.character_id));
  vm_.SetMember(self, "_x", AsValue::Number(ToPixels(place.matrix.translate_x)));
  vm_.SetMember(self, "_y", AsValue::Number(ToPixels(place.matrix.translate_y)));
  vm_.SetMember(self, "_width", AsValue::Number(ToPixels(bounds.width())));
  vm_.SetMember(self, "_height", AsValue::Number(ToPixels(bounds.height())));
  const f32 alpha = place.has_color_transform ? place.color_transform.mul_a : 1.0f;
  if (alpha >= 1.0f)
    place_alpha_.erase(self.object());
  else
    place_alpha_[self.object()] = alpha < 0 ? 0.0f : alpha;
}

f32 Stage::PlacedAlpha(const AsValue& clip) const {
  if (!clip.is_object())
    return 1.0f;
  const f32* alpha = place_alpha_.find(clip.object());
  return alpha ? *alpha : 1.0f;
}

u32 Stage::BuildClip(const Timeline& timeline, u32 parent, base::StringRef name,
                     u16 character, u32 depth) {
  // The class the movie bound to this symbol, if it bound one. Its prototype
  // becomes the clip's, so the clip answers to the methods its movie wrote.
  AsValue klass = AsValue::Undefined();
  if (const base::String* symbol = movie_.exports.find(character))
    klass = vm_.RegisteredClass(*symbol);
  u32 prototype = vm_.movie_clip_prototype();
  if (klass.is_object()) {
    const AsValue proto = vm_.GetMember(klass, "prototype");
    if (proto.is_object())
      prototype = proto.object();
  }

  const u32 clip = vm_.NewObject(prototype);
  vm_.Get(clip).is_movie_clip = true;
  ++clip_count_;
  const AsValue self = AsValue::Obj(clip);
  vm_.SetMember(self, "_name", AsValue::Str(name));
  vm_.SetMember(self, "_visible", AsValue::Bool(true));
  vm_.SetMember(self, "_alpha", AsValue::Number(100));
  vm_.SetMember(self, "_x", AsValue::Number(0));
  vm_.SetMember(self, "_y", AsValue::Number(0));
  if (parent != 0)
    vm_.SetMember(self, "_parent", AsValue::Obj(parent));

  // Registered before its children, so a child's goto can find this clip.
  ClipState state;
  state.timeline = &timeline;
  state.object = clip;
  clips_.push_back(base::move(state));
  const u32 index = static_cast<u32>(clips_.size() - 1);
  vm_.Get(clip).host = index + 1;  // 1-based; 0 means "not a stage clip"
  vm_.SetMember(self, "_totalframes",
                AsValue::Number(static_cast<f64>(timeline.frames.size())));
  vm_.SetMember(self, "_currentframe", AsValue::Number(1));

  if (depth < kMaxDepth)
    ApplyFrame(index, 0, depth);

  // The constructor last, so it finds its children already in place - which is
  // what the movie's own code expects (`this.MainList` in a constructor body).
  if (klass.is_object()) {
    ++classed_count_;
    vm_.Call(klass, self, base::Vector<AsValue>());
  } else if (const base::String* symbol = movie_.exports.find(character)) {
    unclassed_.push_back(base::Format("{} <- {}", name, *symbol));
  }
  // The constructor is where a class installs its own onLoad, so the event goes
  // out after it rather than before. During the first build it is queued
  // instead: the player runs a frame's actions before it dispatches that
  // frame's load events, and the root's actions are what install the helpers a
  // menu's onLoad calls (TextField.prototype.SetText among them).
  if (running_)
    Dispatch(self, "onLoad");
  else
    pending_load_.push_back(clip);
  return clip;
}

void Stage::ApplyFrame(u32 state_index, u32 frame, u32 depth) {
  if (state_index >= clips_.size())
    return;
  // Copied, not referenced: building children pushes onto clips_, which can
  // reallocate underneath a reference held across the loop.
  const Timeline* timeline = clips_[state_index].timeline;
  const u32 object = clips_[state_index].object;
  if (!timeline)
    return;
  const AsValue self = AsValue::Obj(object);
  const base::Vector<Place> list = DisplayListAt(*timeline, frame);
  const base::Vector<base::Pair<u16, base::String>> previous = clips_[state_index].placed;

  // Drop what this frame no longer places. A clip the script still holds on to
  // keeps working; it is only unreachable by name from its parent, which is
  // what leaving the display list means.
  for (const auto& had : previous) {
    bool still_there = false;
    for (const Place& place : list) {
      if (place.has_character && place.depth == had.first && NameFor(place) == had.second)
        still_there = true;
    }
    if (!still_there)
      vm_.SetMember(self, had.second, AsValue::Undefined());
  }

  base::Vector<base::Pair<u16, base::String>> now;
  for (const Place& place : list) {
    if (!place.has_character)
      continue;
    const base::String name = NameFor(place);
    now.push_back(base::Pair<u16, base::String>{place.depth, name});

    // Already there at the same depth and name: leave it alone, so a clip keeps
    // whatever state its own script gave it across a frame change.
    bool kept = false;
    for (const auto& had : previous) {
      if (had.first == place.depth && had.second == name)
        kept = true;
    }
    if (kept) {
      // Kept, but the frame can still have moved it or faded it: a fader is a
      // timeline that does nothing else. So the placement is re-read even
      // though the object stays.
      const AsValue existing = vm_.GetMember(self, name);
      if (existing.is_object()) {
        PlaceGeometry(existing, place);
        continue;
      }
    }

    if (const Timeline* sprite = movie_.FindSprite(place.character_id)) {
      if (depth >= kMaxDepth)
        continue;
      const u32 child = BuildClip(*sprite, object, name, place.character_id, depth + 1);
      PlaceGeometry(AsValue::Obj(child), place);
      vm_.SetMember(self, name, AsValue::Obj(child));
      continue;
    }
    // A text field is the other thing a script addresses by name; give it
    // enough shape that reads and writes of its text land somewhere.
    if (const EditText* text = movie_.FindEditText(place.character_id)) {
      const u32 field = vm_.NewObject(vm_.text_field_prototype());
      const AsValue field_value = AsValue::Obj(field);
      vm_.SetMember(field_value, "_name", AsValue::Str(name));
      // `text` is the plain string and `htmlText` the markup, as the language
      // splits them. A DefineEditText carries the markup, so the plain form has
      // to be stripped out or every script that reads `.text` gets a tag soup.
      vm_.SetMember(field_value, "text", AsValue::Str(StripHtml(text->initial_text)));
      vm_.SetMember(field_value, "htmlText", AsValue::Str(text->initial_text));
      vm_.SetMember(field_value, "_parent", self);
      PlaceGeometry(field_value, place);
      vm_.SetMember(self, name, field_value);
    }
  }

  for (const auto& entry : now)
    clips_[state_index].high_depth =
        base::Max<i32>(clips_[state_index].high_depth, static_cast<i32>(entry.first));
  clips_[state_index].placed = base::move(now);
  clips_[state_index].frame = frame;
  vm_.SetMember(self, "_currentframe", AsValue::Number(static_cast<f64>(frame + 1)));
}

void Stage::PlacedNames(const AsValue& clip, base::Vector<base::String>& current,
                        base::Vector<base::String>& ever) const {
  const u32 index = StateIndexOf(clip);
  if (index == 0 || index > clips_.size())
    return;
  const ClipState& state = clips_[index - 1];
  for (const auto& entry : state.placed)
    current.push_back(entry.second);
  if (!state.timeline)
    return;
  for (const Frame& frame : state.timeline->frames) {
    for (const Place& place : frame.places) {
      if (!place.has_character)
        continue;
      const base::String name = NameFor(place);
      bool known = false;
      for (const base::String& had : ever)
        known = known || had == name;
      if (!known)
        ever.push_back(name);
    }
  }
}

bool Stage::Goto(const AsValue& clip, u32 frame) {
  const u32 index = StateIndexOf(clip);
  if (index == 0 || index > clips_.size())
    return false;
  const u32 state = index - 1;
  const Timeline* timeline = clips_[state].timeline;
  if (!timeline || timeline->frames.empty())
    return false;
  if (frame >= timeline->frames.size())
    frame = static_cast<u32>(timeline->frames.size() - 1);
  ++goto_count_;
  ApplyFrame(state, frame, 0);
  return true;
}

bool Stage::GotoLabel(const AsValue& clip, base::StringRef label) {
  const u32 index = StateIndexOf(clip);
  if (index == 0 || index > clips_.size())
    return false;
  const Timeline* timeline = clips_[index - 1].timeline;
  if (!timeline)
    return false;
  for (mem_size i = 0; i < timeline->frames.size(); ++i) {
    if (timeline->frames[i].label == label)
      return Goto(clip, static_cast<u32>(i));
  }
  return false;
}

AsValue Stage::Attach(const AsValue& parent, base::StringRef symbol,
                      base::StringRef name, i32 depth) {
  const u32 index = StateIndexOf(parent);
  if (index == 0 || index > clips_.size())
    return AsValue::Undefined();
  u16 character = 0;
  for (const auto& entry : movie_.exports) {
    if (entry.value == symbol) {
      character = entry.key;
      break;
    }
  }
  const Timeline* sprite = character != 0 ? movie_.FindSprite(character) : nullptr;
  if (!sprite)
    return AsValue::Undefined();

  const u32 owner = clips_[index - 1].object;
  const u32 child = BuildClip(*sprite, owner, name, character, 1);
  vm_.SetMember(AsValue::Obj(owner), name, AsValue::Obj(child));
  clips_[index - 1].high_depth = base::Max(clips_[index - 1].high_depth, depth);
  // Recorded in the parent's display list so a later frame change knows the
  // clip is there and does not build a second one over the top of it.
  clips_[index - 1].placed.push_back(
      base::Pair<u16, base::String>{static_cast<u16>(depth), base::String(name)});
  return AsValue::Obj(child);
}

AsValue Stage::CreateEmpty(const AsValue& parent, base::StringRef name, i32 depth) {
  const u32 index = StateIndexOf(parent);
  if (index == 0 || index > clips_.size())
    return AsValue::Undefined();
  const u32 owner = clips_[index - 1].object;
  // An empty clip has no timeline of its own; it exists to hold what a script
  // puts inside it.
  const u32 child = vm_.NewObject(vm_.movie_clip_prototype());
  vm_.Get(child).is_movie_clip = true;
  ++clip_count_;
  const AsValue self = AsValue::Obj(child);
  vm_.SetMember(self, "_name", AsValue::Str(name));
  vm_.SetMember(self, "_visible", AsValue::Bool(true));
  vm_.SetMember(self, "_alpha", AsValue::Number(100));
  vm_.SetMember(self, "_x", AsValue::Number(0));
  vm_.SetMember(self, "_y", AsValue::Number(0));
  vm_.SetMember(self, "_parent", AsValue::Obj(owner));
  ClipState state;
  state.object = child;
  clips_.push_back(base::move(state));
  vm_.Get(child).host = clips_.size();

  vm_.SetMember(AsValue::Obj(owner), name, self);
  clips_[index - 1].high_depth = base::Max(clips_[index - 1].high_depth, depth);
  clips_[index - 1].placed.push_back(
      base::Pair<u16, base::String>{static_cast<u16>(depth), base::String(name)});
  return self;
}

AsValue Stage::Duplicate(const AsValue& clip, base::StringRef name, i32 depth) {
  const u32 index = StateIndexOf(clip);
  if (index == 0 || index > clips_.size())
    return AsValue::Undefined();
  const Timeline* timeline = clips_[index - 1].timeline;
  const AsValue parent = vm_.GetMember(clip, "_parent");
  const u32 parent_index = StateIndexOf(parent);
  if (!timeline || parent_index == 0 || parent_index > clips_.size())
    return AsValue::Undefined();
  const u32 owner = clips_[parent_index - 1].object;

  // The character the original came from, so the copy gets the same class.
  u16 character = 0;
  for (const auto& entry : movie_.characters) {
    if (entry.value.kind == CharacterKind::kSprite &&
        &movie_.sprites[entry.value.index] == timeline) {
      character = entry.key;
      break;
    }
  }
  const u32 copy = BuildClip(*timeline, owner, name, character, 1);
  vm_.SetMember(AsValue::Obj(owner), name, AsValue::Obj(copy));
  clips_[parent_index - 1].high_depth = base::Max(clips_[parent_index - 1].high_depth, depth);
  clips_[parent_index - 1].placed.push_back(
      base::Pair<u16, base::String>{static_cast<u16>(depth), base::String(name)});
  return AsValue::Obj(copy);
}

void Stage::SetPlaying(const AsValue& clip, bool playing) {
  const u32 index = StateIndexOf(clip);
  if (index != 0 && index <= clips_.size())
    clips_[index - 1].playing = playing;
}

i32 Stage::NextDepth(const AsValue& clip) const {
  const u32 index = StateIndexOf(clip);
  if (index == 0 || index > clips_.size())
    return 0;
  return clips_[index - 1].high_depth + 1;
}

bool Stage::Remove(const AsValue& clip) {
  const u32 index = StateIndexOf(clip);
  if (index == 0 || index > clips_.size())
    return false;
  const AsValue parent = vm_.GetMember(clip, "_parent");
  const u32 parent_index = StateIndexOf(parent);
  const base::String name = vm_.ToString(vm_.GetMember(clip, "_name"));
  if (parent_index != 0 && parent_index <= clips_.size()) {
    base::Vector<base::Pair<u16, base::String>>& placed = clips_[parent_index - 1].placed;
    for (mem_size i = 0; i < placed.size(); ++i) {
      if (placed[i].second == name) {
        placed.erase(i);
        break;
      }
    }
    vm_.SetMember(parent, name, AsValue::Undefined());
  }
  clips_[index - 1].timeline = nullptr;  // stops it playing once detached
  return true;
}

namespace {

// The timeline API a menu drives its states with. Frames are 1-based in the
// language and 0-based here.
AsValue ClipGoto(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  Stage* stage = static_cast<Stage*>(vm.host());
  if (!stage || args.empty() || HostIndex(vm, self) == 0)
    return AsValue::Undefined();
  if (args[0].is_string()) {
    // A label that is not on this timeline is not an error in the language: the
    // clip stays where it is.
    stage->GotoLabel(self, args[0].string());
    return AsValue::Undefined();
  }
  const f64 frame = vm.ToNumber(args[0]);
  stage->Goto(self, frame < 1 ? 0 : static_cast<u32>(frame) - 1);
  return AsValue::Undefined();
}

AsValue ClipGotoAndPlay(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  ClipGoto(vm, self, args);
  if (Stage* stage = static_cast<Stage*>(vm.host()))
    stage->SetPlaying(self, true);
  return AsValue::Undefined();
}

AsValue ClipNextFrame(Vm& vm, const AsValue& self, const base::Vector<AsValue>&) {
  Stage* stage = static_cast<Stage*>(vm.host());
  if (!stage || HostIndex(vm, self) == 0)
    return AsValue::Undefined();
  const f64 current = vm.ToNumber(vm.GetMember(self, "_currentframe"));
  stage->Goto(self, static_cast<u32>(current));  // 1-based current is the next 0-based
  return AsValue::Undefined();
}

AsValue ClipPrevFrame(Vm& vm, const AsValue& self, const base::Vector<AsValue>&) {
  Stage* stage = static_cast<Stage*>(vm.host());
  if (!stage || HostIndex(vm, self) == 0)
    return AsValue::Undefined();
  const f64 current = vm.ToNumber(vm.GetMember(self, "_currentframe"));
  if (current > 1)
    stage->Goto(self, static_cast<u32>(current) - 2);
  return AsValue::Undefined();
}

AsValue ClipAttachMovie(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  Stage* stage = static_cast<Stage*>(vm.host());
  if (!stage || args.size() < 2)
    return AsValue::Undefined();
  const i32 depth = args.size() > 2 ? static_cast<i32>(vm.ToNumber(args[2])) : 0;
  const AsValue clip =
      stage->Attach(self, vm.ToString(args[0]), vm.ToString(args[1]), depth);
  // attachMovie's fourth argument is an object whose members are copied onto
  // the new clip before its constructor would see them.
  if (clip.is_object() && args.size() > 3 && args[3].is_object() &&
      vm.Valid(args[3].object())) {
    const base::Vector<base::String> keys = vm.Get(args[3].object()).order;
    for (const base::String& key : keys)
      vm.SetMember(clip, key, vm.GetMember(args[3], key));
  }
  return clip;
}

AsValue ClipGetNextHighestDepth(Vm& vm, const AsValue& self,
                                const base::Vector<AsValue>&) {
  Stage* stage = static_cast<Stage*>(vm.host());
  if (!stage)
    return AsValue::Number(0);
  return AsValue::Number(static_cast<f64>(stage->NextDepth(self)));
}

AsValue ClipRemove(Vm& vm, const AsValue& self, const base::Vector<AsValue>&) {
  Stage* stage = static_cast<Stage*>(vm.host());
  if (stage)
    stage->Remove(self);
  return AsValue::Undefined();
}

AsValue ClipCreateEmpty(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  Stage* stage = static_cast<Stage*>(vm.host());
  if (!stage || args.empty())
    return AsValue::Undefined();
  const i32 depth = args.size() > 1 ? static_cast<i32>(vm.ToNumber(args[1])) : 0;
  return stage->CreateEmpty(self, vm.ToString(args[0]), depth);
}

AsValue ClipPlay(Vm& vm, const AsValue& self, const base::Vector<AsValue>&) {
  if (Stage* stage = static_cast<Stage*>(vm.host()))
    stage->SetPlaying(self, true);
  return AsValue::Undefined();
}

AsValue ClipStop(Vm& vm, const AsValue& self, const base::Vector<AsValue>&) {
  if (Stage* stage = static_cast<Stage*>(vm.host()))
    stage->SetPlaying(self, false);
  return AsValue::Undefined();
}

AsValue ClipDuplicate(Vm& vm, const AsValue& self, const base::Vector<AsValue>& args) {
  Stage* stage = static_cast<Stage*>(vm.host());
  if (!stage || args.empty())
    return AsValue::Undefined();
  const i32 depth = args.size() > 1 ? static_cast<i32>(vm.ToNumber(args[1])) : 0;
  return stage->Duplicate(self, vm.ToString(args[0]), depth);
}

}  // namespace

void Stage::InstallClipApi() {
  const AsValue proto = AsValue::Obj(vm_.movie_clip_prototype());
  vm_.SetMember(proto, "gotoAndStop", AsValue::Obj(vm_.NewNative(ClipGoto)));
  vm_.SetMember(proto, "gotoAndPlay", AsValue::Obj(vm_.NewNative(ClipGotoAndPlay)));
  vm_.SetMember(proto, "nextFrame", AsValue::Obj(vm_.NewNative(ClipNextFrame)));
  vm_.SetMember(proto, "prevFrame", AsValue::Obj(vm_.NewNative(ClipPrevFrame)));
  vm_.SetMember(proto, "play", AsValue::Obj(vm_.NewNative(ClipPlay)));
  vm_.SetMember(proto, "stop", AsValue::Obj(vm_.NewNative(ClipStop)));
  vm_.SetMember(proto, "duplicateMovieClip", AsValue::Obj(vm_.NewNative(ClipDuplicate)));
  vm_.SetMember(proto, "attachMovie", AsValue::Obj(vm_.NewNative(ClipAttachMovie)));
  vm_.SetMember(proto, "getNextHighestDepth",
                AsValue::Obj(vm_.NewNative(ClipGetNextHighestDepth)));
  vm_.SetMember(proto, "removeMovieClip", AsValue::Obj(vm_.NewNative(ClipRemove)));
  vm_.SetMember(proto, "unloadMovie", AsValue::Obj(vm_.NewNative(ClipRemove)));
  vm_.SetMember(proto, "createEmptyMovieClip",
                AsValue::Obj(vm_.NewNative(ClipCreateEmpty)));
}

void Stage::Run() {
  vm_.set_host(this);
  InstallClipApi();

  // Everything the library defines runs before the stage is built: the init
  // blocks, and the frame scripts of the sprites that are only there to carry
  // code. The AS2 compiler puts a class in a `__Packages.<name>` sprite that is
  // never placed, and its definition has to be in hand before a clip asks for
  // it - registering a class the movie has not defined yet silently binds
  // nothing, and the clip comes out as a bare movie clip.
  base::Vector<u32> root_scripts;
  for (const Script& script : movie_.scripts) {
    if (script.code.empty())
      continue;
    const u32 index = vm_.AddScript(script.code);
    if (script.kind == Script::Kind::kFrame && script.timeline_id == 0)
      root_scripts.push_back(index);
    else
      vm_.Run(index, AsValue::Undefined());
  }

  const u32 root = BuildClip(movie_.root, 0, "_level0", 0, 0);
  root_ = AsValue::Obj(root);
  vm_.set_root(root_);

  for (u32 index : root_scripts)
    vm_.Run(index, root_);

  // Children first: a page's onLoad fills the list inside it, which only works
  // once that list's own onLoad has sized it.
  running_ = true;
  const base::Vector<u32> loading = base::move(pending_load_);
  for (mem_size i = loading.size(); i > 0; --i)
    Dispatch(AsValue::Obj(loading[i - 1]), "onLoad");
}

bool Stage::Dispatch(const AsValue& clip, base::StringRef handler,
                     const base::Vector<AsValue>& args) {
  const AsValue fn = vm_.GetMember(clip, handler);
  if (!fn.is_object() || !vm_.Valid(fn.object()) || !vm_.Get(fn.object()).is_function)
    return false;
  vm_.Call(fn, clip, args);
  return true;
}

bool Stage::Dispatch(const AsValue& clip, base::StringRef handler) {
  return Dispatch(clip, handler, base::Vector<AsValue>());
}

u32 Stage::Broadcast(base::StringRef handler) {
  u32 ran = 0;
  // The object ids are copied first: a handler can attach or remove clips,
  // which moves the table underneath the walk.
  base::Vector<u32> targets;
  for (const ClipState& state : clips_)
    targets.push_back(state.object);
  for (u32 object : targets) {
    if (!vm_.Valid(object))
      continue;
    if (Dispatch(AsValue::Obj(object), handler))
      ++ran;
  }
  return ran;
}

u32 Stage::Tick(f64 elapsed_ms) {
  const u32 timers = vm_.Tick(elapsed_ms);

  // Advance whatever is playing, one frame and wrapping. Collected first
  // because applying a frame can build clips and move the table.
  base::Vector<u32> playing;
  for (mem_size i = 0; i < clips_.size(); ++i)
    if (clips_[i].playing && clips_[i].timeline && clips_[i].timeline->frames.size() > 1)
      playing.push_back(static_cast<u32>(i));
  for (u32 index : playing) {
    if (index >= clips_.size())
      continue;
    const mem_size frames = clips_[index].timeline->frames.size();
    const u32 next = static_cast<u32>((clips_[index].frame + 1) % frames);
    ++goto_count_;
    ApplyFrame(index, next, 0);
  }

  return timers + static_cast<u32>(playing.size()) + Broadcast("onEnterFrame");
}

base::Vector<AsValue> Stage::StatefulClips() const {
  base::Vector<AsValue> out;
  for (const ClipState& state : clips_) {
    if (!state.timeline)
      continue;
    bool labelled = false;
    for (const Frame& frame : state.timeline->frames)
      labelled = labelled || !frame.label.empty();
    if (labelled)
      out.push_back(AsValue::Obj(state.object));
  }
  return out;
}

base::Vector<base::String> Stage::LabelsOf(const AsValue& clip) const {
  base::Vector<base::String> out;
  const u32 index = StateIndexOf(clip);
  if (index == 0 || index > clips_.size())
    return out;
  const Timeline* timeline = clips_[index - 1].timeline;
  if (!timeline)
    return out;
  for (const Frame& frame : timeline->frames)
    if (!frame.label.empty())
      out.push_back(frame.label);
  return out;
}

AsValue Stage::Find(base::StringRef path) const {
  AsValue current = root_;
  base::String segment;
  for (mem_size i = 0; i <= path.size(); ++i) {
    const char c = i < path.size() ? path[i] : '.';
    if (c != '.') {
      segment.push_back(c);
      continue;
    }
    if (segment.empty())
      continue;
    current = const_cast<Vm&>(vm_).GetMember(current, segment);
    segment = base::String();
    if (!current.is_object())
      return AsValue::Undefined();
  }
  return current;
}

}  // namespace rx::swf
