#include "components/swf/bridge.h"

#include <base/strings/format.h>

namespace rx::swf {
namespace {

// Walks _global.gfx.io.GameDelegate. Undefined before the movie's library code
// has run, which is the normal state for a movie that carries no menu class.
AsValue Path(Vm& vm, base::StringRef a, base::StringRef b, base::StringRef c) {
  const AsValue gfx = vm.GetMember(AsValue::Obj(vm.global()), a);
  if (!gfx.is_object())
    return AsValue::Undefined();
  const AsValue io = vm.GetMember(gfx, b);
  if (!io.is_object())
    return AsValue::Undefined();
  return vm.GetMember(io, c);
}

bool IsFunction(Vm& vm, const AsValue& value) {
  return value.is_object() && vm.Valid(value.object()) && vm.Get(value.object()).is_function;
}

}  // namespace

GameBridge::GameBridge(Vm& vm) : vm_(vm) {
  vm_.set_external_handler(&GameBridge::Trampoline, this);
}

AsValue GameBridge::Trampoline(void* user, Vm&, base::StringRef name,
                               const base::Vector<AsValue>& args) {
  return static_cast<GameBridge*>(user)->Handle(name, args);
}

AsValue GameBridge::Delegate() {
  return Path(vm_, "gfx", "io", "GameDelegate");
}

// GameDelegate.call unshifts the method name and a response id onto the
// arguments before handing them to ExternalInterface, and the Vm has already
// taken the name off. So args[0] is the id and the call's own arguments follow.
AsValue GameBridge::Handle(base::StringRef name, const base::Vector<AsValue>& args) {
  Call call;
  call.name = base::String(name);
  if (!args.empty())
    call.id = static_cast<u32>(vm_.ToNumber(args[0]));
  for (mem_size i = 1; i < args.size(); ++i)
    call.args.push_back(args[i]);

  const AsValue* answer = answers_.find(call.name);
  if (answer == nullptr) {
    pending_.push_back(base::move(call));
    return AsValue::Undefined();
  }
  base::Vector<AsValue> reply;
  reply.push_back(*answer);
  Respond(call.id, reply);
  return *answer;
}

void GameBridge::SetAnswer(base::StringRef name, const AsValue& value) {
  answers_[base::String(name)] = value;
}

u32 GameBridge::Open() {
  u32 opened = 0;
  // Snapshot the count: InitExtensions attaches clips of its own, and those
  // have already had theirs run by the class that made them.
  const u32 count = vm_.object_count();
  for (u32 i = 1; i < count; ++i) {
    if (!vm_.Valid(i) || !vm_.Get(i).is_movie_clip)
      continue;
    const AsValue clip = AsValue::Obj(i);
    const AsValue init = vm_.GetMember(clip, "InitExtensions");
    if (!IsFunction(vm_, init))
      continue;
    vm_.Call(init, clip, base::Vector<AsValue>());
    ++opened;
  }
  return opened;
}

bool GameBridge::Invoke(base::StringRef name, const base::Vector<AsValue>& args) {
  const AsValue delegate = Delegate();
  if (!delegate.is_object())
    return false;
  // receiveCall silently returns when nothing is registered, so check the table
  // first: a host wants to know that a menu is not listening for what it sent.
  const AsValue table = vm_.GetMember(delegate, "callBackHash");
  if (!table.is_object() || !vm_.GetMember(table, name).is_object())
    return false;
  const AsValue receive = vm_.GetMember(delegate, "receiveCall");
  if (!IsFunction(vm_, receive))
    return false;
  base::Vector<AsValue> all;
  all.push_back(AsValue::Str(name));
  for (const AsValue& arg : args)
    all.push_back(arg);
  vm_.Call(receive, delegate, all);
  return true;
}

bool GameBridge::Invoke(base::StringRef name) {
  return Invoke(name, base::Vector<AsValue>());
}

bool GameBridge::Respond(u32 id, const base::Vector<AsValue>& args) {
  const AsValue delegate = Delegate();
  if (!delegate.is_object())
    return false;
  const AsValue receive = vm_.GetMember(delegate, "receiveResponse");
  if (!IsFunction(vm_, receive))
    return false;
  base::Vector<AsValue> all;
  all.push_back(AsValue::Number(static_cast<f64>(id)));
  for (const AsValue& arg : args)
    all.push_back(arg);
  vm_.Call(receive, delegate, all);
  return true;
}

base::Vector<base::String> GameBridge::Callbacks() {
  base::Vector<base::String> out;
  const AsValue delegate = Delegate();
  if (!delegate.is_object())
    return out;
  const AsValue table = vm_.GetMember(delegate, "callBackHash");
  if (!table.is_object() || !vm_.Valid(table.object()))
    return out;
  for (const base::String& key : vm_.Get(table.object()).order) {
    const AsValue entry = vm_.GetMember(table, key);
    if (entry.is_object())
      out.push_back(key);
  }
  return out;
}

AsValue GameBridge::Array(const base::Vector<AsValue>& items) {
  const AsValue out = AsValue::Obj(vm_.NewArray());
  for (mem_size i = 0; i < items.size(); ++i)
    vm_.SetMember(out, base::Format("{}", i), items[i]);
  vm_.SetMember(out, "length", AsValue::Number(static_cast<f64>(items.size())));
  return out;
}

}  // namespace rx::swf
