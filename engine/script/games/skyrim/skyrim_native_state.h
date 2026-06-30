#ifndef RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVE_STATE_H_
#define RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVE_STATE_H_

#include <string>

#include "core/types.h"
#include "script/papyrus/value.h"

// A keyed runtime store the native batches share, so a set/get pair round-trips
// even when the engine subsystem behind it is not built yet (a script that calls
// SetGhost(true) then IsGhost() sees true). State is keyed by an object's handle
// and a name the caller picks. The guest runs single-threaded, so no locking.
namespace rec::script::skyrim::state {

using papyrus::ObjectRef;

bool GetFlag(ObjectRef owner, const std::string& key, bool fallback = false);
void SetFlag(ObjectRef owner, const std::string& key, bool value);

i32 GetInt(ObjectRef owner, const std::string& key, i32 fallback = 0);
void SetInt(ObjectRef owner, const std::string& key, i32 value);

f32 GetFloat(ObjectRef owner, const std::string& key, f32 fallback = 0.0f);
void SetFloat(ObjectRef owner, const std::string& key, f32 value);

ObjectRef GetRef(ObjectRef owner, const std::string& key);
void SetRef(ObjectRef owner, const std::string& key, ObjectRef value);

// Set-valued state, for the collection natives (an actor's perks, spells, shouts).
bool HasMember(ObjectRef owner, const std::string& key, ObjectRef member);
void AddMember(ObjectRef owner, const std::string& key, ObjectRef member);
void RemoveMember(ObjectRef owner, const std::string& key, ObjectRef member);
i32 MemberCount(ObjectRef owner, const std::string& key);

// Drops every stored value for one object (ObjectReference.Reset, ResetQuest).
void Clear(ObjectRef owner);

}  // namespace rec::script::skyrim::state

#endif  // RECREATION_SCRIPT_GAMES_SKYRIM_SKYRIM_NATIVE_STATE_H_
