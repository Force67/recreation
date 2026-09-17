#ifndef RECREATION_MASTERLIST_SERVER_INFO_H_
#define RECREATION_MASTERLIST_SERVER_INFO_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace rx::masterlist {

// What a host says about itself when it announces. The address is deliberately
// absent: a host supplies only its port and the list takes the IP from the
// socket, so nobody can announce somebody else's machine into the browser.
struct ServerInfo {
  u16 port = 0;
  base::String name;
  base::String gametype;  // "campaign-coop", "roleplay", "racing", "sandbox"
  base::String domain;    // "skyrim", "fallout4", "starfield"
  base::String mode_id;   // the gamemode manifest armed, empty for the base rules
  base::String version;   // the build a joiner has to match
  // Digest of the host's load order, and how many plugins went into it. The
  // client compares both before dialling, which is what turns "failed to load"
  // into "you are missing two plugins".
  base::String plugins;
  u32 plugin_count = 0;
  u32 players = 0;
  u32 max_players = 0;
  // What a joiner will have to stream before it can play.
  u32 resources = 0;
  u64 resources_bytes = 0;
  bool passworded = false;
  base::Vector<base::String> tags;
};

// One server as the browser reads it back. Everything in ServerInfo, plus what
// only the list knows: where it is, how stale the entry is, and whether the
// list reached the port itself.
struct ServerEntry {
  base::String address;  // "ip:port", the string to dial
  base::String name;
  base::String gametype;
  base::String domain;
  base::String mode_id;
  base::String version;
  base::String plugins;
  u32 plugin_count = 0;
  u32 players = 0;
  u32 max_players = 0;
  u32 resources = 0;
  u64 resources_bytes = 0;
  u64 age_secs = 0;     // since the last heartbeat; fresh sorts over stale
  u64 uptime_secs = 0;
  bool passworded = false;
  // The list answered a UDP probe on the announced port. False means "not
  // checked" as well as "did not answer", so it is a state to show, not a
  // failure to hide.
  bool verified = false;
  base::Vector<base::String> tags;

  bool has_slots() const { return players < max_players; }
};

// Filters the list applies server-side, so a full browser never ships more
// than the screen can use. Every field is optional; an empty query lists
// everything the list will return in one page.
struct ListQuery {
  base::String domain;
  base::String gametype;
  base::String mode_id;
  base::String text;  // case-insensitive substring of the name
  bool has_slots = false;
  bool no_password = false;
  bool verified_only = false;
  u32 limit = 0;   // 0 leaves the list's own default
  u32 offset = 0;
};

}  // namespace rx::masterlist

#endif  // RECREATION_MASTERLIST_SERVER_INFO_H_
