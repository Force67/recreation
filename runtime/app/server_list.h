#ifndef RECREATION_RUNTIME_APP_SERVER_LIST_H_
#define RECREATION_RUNTIME_APP_SERVER_LIST_H_

#include <base/strings/xstring.h>

// The game's use of the server list: what a hosted session tells the list about
// itself, and what the Join screen shows of everybody else's. The list client
// itself is components/masterlist; this is the wiring that knows about games,
// load orders and the menu.
//
// Nothing here can take a session down. An unreachable list leaves the host
// running and unlisted (with the reason in Announcer::status) and the browser
// empty with a line saying why, because "nobody is hosting" and "we could not
// ask" must never look the same to the player.

namespace rx {

class Engine;

#if RECREATION_HAS_NET

// The list this build talks to: --masterlist, then RX_MASTERLIST, then nothing.
// Empty means no list at all.
base::String MasterlistUrl(const Engine& engine);

// Announces a hosted session and keeps it listed. A no-op unless the session
// was asked to be public and a list is configured.
void StartServerAnnounce(Engine& engine);
// Retires the entry now rather than letting it age out of the browser.
void StopServerAnnounce(Engine& engine);

// Asks for the browser's contents on a worker thread, and picks the answer up.
// Both are safe to call every frame: the query is skipped while one is in
// flight, the poll does nothing until an answer has landed.
void RequestServerList(Engine& engine);
void PollServerList(Engine& engine);

#endif  // RECREATION_HAS_NET

}  // namespace rx

#endif  // RECREATION_RUNTIME_APP_SERVER_LIST_H_
