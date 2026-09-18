#include "components/gamenet/address.h"

namespace rx::net {
namespace {

// Digits only, and the whole suffix has to be one: atoi would read "80junk" as
// 80, and on a value past INT_MAX it is undefined outright (glibc truncates,
// so "4294997596" arrives as port 30300).
bool ParsePort(const base::String& text, u16* out) {
  if (text.empty() || text.size() > 5)
    return false;
  u32 value = 0;
  for (mem_size i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9')
      return false;
    value = value * 10 + static_cast<u32>(c - '0');
  }
  if (value == 0 || value > 65535)
    return false;
  *out = static_cast<u16>(value);
  return true;
}

}  // namespace

bool SplitHostPort(const base::String& address, base::String* host, u16* port) {
  if (address.empty())
    return false;

  if (address[0] == '[') {
    const mem_size close = address.find(']');
    if (close == base::String::npos || close == 1)
      return false;
    *host = address.substr(1, close - 1);
    const base::String rest = address.substr(close + 1);
    if (rest.empty())
      return true;
    return rest[0] == ':' && ParsePort(rest.substr(1), port);
  }

  const mem_size colon = address.find(':');
  if (colon == base::String::npos) {
    *host = address;
    return true;
  }
  // A second colon outside brackets is a bare v6 literal. There is no port to
  // take off it, so the whole string is the host.
  if (address.find(':', colon + 1) != base::String::npos) {
    *host = address;
    return true;
  }
  if (colon == 0)
    return false;  // ":29700" names no host
  if (!ParsePort(address.substr(colon + 1), port))
    return false;
  *host = address.substr(0, colon);
  return true;
}

}  // namespace rx::net
