#ifndef RECREATION_MASTERLIST_MASTERLIST_CLIENT_H_
#define RECREATION_MASTERLIST_MASTERLIST_CLIENT_H_

#include <base/strings/xstring.h>

#include "components/masterlist/server_info.h"

// The game's side of the server list: announce a hosted session, keep it alive,
// take it down, and read back what everyone else is hosting.
//
// The list itself is a separate service (masterlist/, its own repo) and the
// wire shape below is the contract with it. The IP is never sent -- a host
// announces a port and the list takes the address from the socket -- so
// nothing here can put somebody else's machine in the browser.

namespace rx::masterlist {

struct AnnounceResult {
  bool ok = false;
  base::String address;  // "ip:port" as the list saw it, which is what joiners dial
  // Bearer for heartbeat and retire, bound to the announcing IP. Re-announcing
  // from the same ip:port keeps the slot and mints a new one.
  base::String token;
  u32 heartbeat_secs = 30;
  u32 entry_ttl_secs = 120;
  base::String error;  // why, when ok is false
};

struct ListResult {
  bool ok = false;
  u32 total = 0;  // matches before paging, so a browser can say "showing 50 of 900"
  base::Vector<ServerEntry> servers;
  base::String error;
};

// One endpoint. Every call BLOCKS for the length of the exchange; Announcer and
// AsyncList are the two ways the game calls it off the frame thread.
class Client {
 public:
  explicit Client(const base::String& base_url);

  // False when the URL is not something the http client can dial, in which
  // case every call fails with that as its error.
  bool valid() const { return valid_; }
  const base::String& base_url() const { return base_url_; }

  AnnounceResult Announce(const ServerInfo& info);
  // `players` is the live count, the only field worth refreshing this often.
  bool Heartbeat(const base::String& token, u32 players, base::String* error);
  bool Retire(const base::String& token, base::String* error);
  ListResult List(const ListQuery& query);

  void set_timeout_ms(u32 ms) { timeout_ms_ = ms; }

 private:
  base::String Endpoint(const char* path) const;

  base::String base_url_;
  bool valid_ = false;
  u32 timeout_ms_ = 8000;
};

// The wire shape, exposed because it is the contract with a service that lives
// in another repo and is the part worth testing without a network.
base::String EncodeAnnounce(const ServerInfo& info);
base::String EncodeListQuery(const ListQuery& query);  // "?domain=skyrim&..." or ""
bool DecodeAnnounce(const base::String& body, AnnounceResult* out);
bool DecodeList(const base::String& body, ListResult* out);
// Pulls the "detail" out of the list's error body, falling back to a plain
// "HTTP 503" when the answer is not one of ours.
base::String DecodeError(u16 status, const base::String& body);

}  // namespace rx::masterlist

#endif  // RECREATION_MASTERLIST_MASTERLIST_CLIENT_H_
