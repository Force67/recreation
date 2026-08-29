#include "components/swf/stage.h"

#include <base/memory/move.h>
#include <base/strings/format.h>

namespace rx::swf {
namespace {

// Nesting cap: a movie can place a sprite inside itself, and the tree is walked
// eagerly rather than on demand.
constexpr u32 kMaxDepth = 24;

// Frame 0's display list, in depth order. The exporter has its own copy of this
// walk that also applies later frames; a menu opens on its first frame, so this
// one only needs the first.
base::Vector<Place> FirstFrame(const Timeline& timeline) {
  base::Vector<Place> list;
  if (timeline.frames.empty())
    return list;
  for (const Place& place : timeline.frames[0].places) {
    mem_size existing = list.size();
    for (mem_size i = 0; i < list.size(); ++i) {
      if (list[i].depth == place.depth) {
        existing = i;
        break;
      }
    }
    if (existing == list.size())
      list.push_back(place);
    else
      list[existing] = place;
  }
  for (mem_size i = 1; i < list.size(); ++i) {
    Place key = base::move(list[i]);
    mem_size j = i;
    while (j > 0 && list[j - 1].depth > key.depth) {
      list[j] = base::move(list[j - 1]);
      --j;
    }
    list[j] = base::move(key);
  }
  return list;
}

}  // namespace

Stage::Stage(Vm& vm, const Movie& movie) : vm_(vm), movie_(movie) {}

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

  if (depth < kMaxDepth) {
    for (const Place& place : FirstFrame(timeline)) {
      if (!place.has_character)
        continue;
      const Timeline* sprite = movie_.FindSprite(place.character_id);
      base::String child_name = place.name;
      if (child_name.empty())
        child_name = base::Format("instance{}", place.depth);
      if (sprite) {
        const u32 child = BuildClip(*sprite, clip, child_name, place.character_id,
                                    depth + 1);
        vm_.SetMember(self, child_name, AsValue::Obj(child));
        continue;
      }
      // A text field is the other thing a script addresses by name; give it
      // enough shape that reads and writes of its text land somewhere.
      if (const EditText* text = movie_.FindEditText(place.character_id)) {
        const u32 field = vm_.NewObject(vm_.movie_clip_prototype());
        const AsValue field_value = AsValue::Obj(field);
        vm_.SetMember(field_value, "_name", AsValue::Str(child_name));
        vm_.SetMember(field_value, "text", AsValue::Str(text->initial_text));
        vm_.SetMember(field_value, "_parent", self);
        vm_.SetMember(self, child_name, field_value);
      }
    }
  }

  // The constructor last, so it finds its children already in place - which is
  // what the movie's own code expects (`this.MainList` in a constructor body).
  if (klass.is_object()) {
    ++classed_count_;
    vm_.Call(klass, self, base::Vector<AsValue>());
  } else if (const base::String* symbol = movie_.exports.find(character)) {
    unclassed_.push_back(base::Format("{} <- {}", name, *symbol));
  }
  return clip;
}

void Stage::Run() {
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
