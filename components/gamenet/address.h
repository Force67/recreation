#ifndef RECREATION_GAMENET_ADDRESS_H_
#define RECREATION_GAMENET_ADDRESS_H_

#include <base/strings/xstring.h>

#include "core/types.h"

namespace rx::net {

// Splits "host:port" into the two the session config takes separately. Every
// address the game can be handed carries its port: a row in the server browser,
// an invite, a --connect the player typed. The session takes host and port as
// separate fields, so getting this wrong dials the right machine on the wrong
// port and the join times out with nothing to blame.
//
// Accepted: "host", "host:port", "[v6]", "[v6]:port", and a bare v6 literal
// with no port ("::1", which has no unambiguous port form). Brackets come off
// the host; the resolver wants the literal, not the notation.
//
// `port` is written only when the address carries one, so the caller seeds it
// with its own default first.
//
// False means the address is not dialable: empty, an empty host, a port that is
// not a number in 1..65535, or unbalanced brackets. A refusal beats guessing,
// because every guess here ends as a connection timeout somewhere else.
bool SplitHostPort(const base::String& address, base::String* host, u16* port);

}  // namespace rx::net

#endif  // RECREATION_GAMENET_ADDRESS_H_
