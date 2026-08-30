#ifndef RECREATION_SWF_BRIDGE_H_
#define RECREATION_SWF_BRIDGE_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include "components/swf/vm.h"

namespace rx::swf {

// The channel between a running menu and the game, in both directions.
//
// A Bethesda menu talks to its host through `gfx.io.GameDelegate`, which the
// movies ship in their own library on top of `ExternalInterface`. It has two
// halves, and only one of them is the menu asking for something:
//
//   movie -> host  `GameDelegate.call(name, args, scope, callback)`, which the
//                  host answers by calling back with the id the call carried.
//   host -> movie  `GameDelegate.addCallBack(name, scope, method)` registers a
//                  handler; `receiveCall(name, ...)` runs it.
//
// The second half is the one that fills a menu. The start menu's option list
// exists because the game sends `sendMenuProperties`, the stats page is blank
// until `SetPlayerInfo` arrives, and a save list is whatever `onSaveLoadBatch`
// put there. So this drives the movie's own dispatchers rather than
// reimplementing them: `Invoke` is `receiveCall`, `Respond` is
// `receiveResponse`, and everything the movie asked for that nobody answered
// queues up in `pending()` for the host to look at.
class GameBridge {
 public:
  // One outgoing call, as the movie made it.
  struct Call {
    base::String name;
    base::Vector<AsValue> args;  // without the name and the response id
    u32 id = 0;                  // what Respond takes, 0 when none was sent
  };

  // Installs itself as the vm's external handler. The vm outlives this.
  explicit GameBridge(Vm& vm);

  // A fixed answer for a query, sent back the moment the movie asks. Enough for
  // the many calls that only read a setting or a platform flag; anything the
  // host has to compute comes off `pending()` and goes back through `Respond`.
  void SetAnswer(base::StringRef name, const AsValue& value);

  // What the game does once a menu's code object is up: calls `InitExtensions`
  // on every clip that has one. Most of a menu's callbacks are registered in
  // there and nothing inside the movie calls it, so a run that skips this
  // leaves the menu listening for almost nothing. Returns how many ran.
  u32 Open();

  // Runs the movie's handler for `name`, which is what the game does when it
  // has something to tell a menu. False when the movie registered none.
  bool Invoke(base::StringRef name, const base::Vector<AsValue>& args);
  bool Invoke(base::StringRef name);

  // Answers a call the movie is waiting on. Only valid while that call is on
  // `pending()`: the movie drops the response slot as soon as it returns.
  bool Respond(u32 id, const base::Vector<AsValue>& args);

  // What the movie asked for and nothing answered, oldest first.
  const base::Vector<Call>& pending() const { return pending_; }
  void ClearPending() { pending_.clear(); }

  // The names the movie is listening for, i.e. what Invoke would reach. Reads
  // GameDelegate's own table, so it is what the movie actually registered.
  base::Vector<base::String> Callbacks();

  // Convenience for building the arguments the game sends.
  AsValue Array(const base::Vector<AsValue>& items);

 private:
  static AsValue Trampoline(void* user, Vm& vm, base::StringRef name,
                            const base::Vector<AsValue>& args);
  AsValue Handle(base::StringRef name, const base::Vector<AsValue>& args);
  AsValue Delegate();

  Vm& vm_;
  base::UnorderedMap<base::String, AsValue> answers_;
  base::Vector<Call> pending_;
};

}  // namespace rx::swf

#endif  // RECREATION_SWF_BRIDGE_H_
