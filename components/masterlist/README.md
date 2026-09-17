# masterlist

Hides **how a session gets into somebody else's server browser, and how the
browser gets filled**. A host announces a port and keeps beating; a client asks
for the list and dials what it picks.

The list itself is a service in its own repo (`masterlist/`, not part of the
game build). This module owns the game's half of that contract:

| File | What it hides |
|------|---------------|
| `server_info.h` | what a host announces, what the browser reads back, what it filters on |
| `masterlist_client.*` | the four calls (announce, heartbeat, retire, list) and their wire shape |
| `json.*` | just enough JSON to read the answers and write the announce |
| `load_order_digest.*` | the fingerprint both sides compute so a join can be refused before it fails |
| `announcer.*` | the host-side worker that keeps an entry alive and retires it |
| `async_list.*` | the client-side worker the Join screen polls |

Two rules the design leans on:

- **The IP never crosses the wire.** A host announces its port; the list takes
  the address from the socket. Nobody can announce somebody else's machine.
- **Being unlisted is never fatal.** A list that is down, slow or refusing
  leaves the session running and unlisted, with the reason in
  `Announcer::status()`. Nothing here can take a server down.

There is no ping field on purpose: only a client can measure its own round
trip, and a number measured on the list's machine would be a lie in the Join
screen's PING column.
