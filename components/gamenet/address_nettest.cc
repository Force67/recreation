// address_nettest: splitting the "host:port" a server browser row, an invite or
// a --connect hands the session. Every wrong answer here is a connection
// timeout somewhere else, with nothing to point at.

#include <cstdio>

#include "components/gamenet/address.h"

using namespace rx;
using rx::net::SplitHostPort;

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

// The caller seeds the port with its own default, which survives an address
// that carries none.
bool Split(const char* address, base::String* host, u16* port) {
  *host = "unset";
  *port = 29700;
  return SplitHostPort(base::String(address), host, port);
}

void TestHostAndPort() {
  base::String host;
  u16 port = 0;

  CHECK(Split("1.2.3.4:29777", &host, &port));
  CHECK(host == base::String("1.2.3.4") && port == 29777);

  CHECK(Split("list.example:8477", &host, &port));
  CHECK(host == base::String("list.example") && port == 8477);

  // No port: the default the caller seeded stands.
  CHECK(Split("1.2.3.4", &host, &port));
  CHECK(host == base::String("1.2.3.4") && port == 29700);

  CHECK(Split("localhost", &host, &port));
  CHECK(host == base::String("localhost") && port == 29700);

  CHECK(Split("1.2.3.4:1", &host, &port));
  CHECK(port == 1);
  CHECK(Split("1.2.3.4:65535", &host, &port));
  CHECK(port == 65535);
}

void TestIpv6() {
  base::String host;
  u16 port = 0;

  // Brackets come off: the resolver wants the literal, not the notation.
  CHECK(Split("[::1]:29777", &host, &port));
  CHECK(host == base::String("::1") && port == 29777);

  CHECK(Split("[2001:db8::1]:8477", &host, &port));
  CHECK(host == base::String("2001:db8::1") && port == 8477);

  CHECK(Split("[::1]", &host, &port));
  CHECK(host == base::String("::1") && port == 29700);

  // A bare literal has no unambiguous port form, so the whole string is the
  // host and the default port stands.
  CHECK(Split("::1", &host, &port));
  CHECK(host == base::String("::1") && port == 29700);

  CHECK(Split("2001:db8::1", &host, &port));
  CHECK(host == base::String("2001:db8::1") && port == 29700);
}

void TestRefusals() {
  base::String host;
  u16 port = 0;

  CHECK(!Split("", &host, &port));
  CHECK(!Split(":29700", &host, &port));      // no host
  CHECK(!Split("host:", &host, &port));       // no port after the colon
  CHECK(!Split("host:0", &host, &port));      // 0 is not dialable
  CHECK(!Split("host:65536", &host, &port));  // past the range
  CHECK(!Split("host:99999", &host, &port));
  CHECK(!Split("host:notaport", &host, &port));
  CHECK(!Split("host:80junk", &host, &port));  // atoi would have said 80
  CHECK(!Split("host: 80", &host, &port));
  CHECK(!Split("host:-1", &host, &port));
  // Past INT_MAX: atoi is undefined here, and glibc's truncation turned this
  // into port 30300 rather than a refusal.
  CHECK(!Split("1.2.3.4:4294997596", &host, &port));
  CHECK(!Split("[::1]:", &host, &port));
  CHECK(!Split("[::1", &host, &port));  // unbalanced
  CHECK(!Split("[]:80", &host, &port));

  // A refusal leaves the caller's values alone rather than half-writing them.
  CHECK(host == base::String("unset") && port == 29700);
}

}  // namespace

int main() {
  std::puts("gamenet address:");
  TestHostAndPort();
  TestIpv6();
  TestRefusals();
  if (g_failures == 0) {
    std::puts("address_nettest: all passed");
    return 0;
  }
  std::printf("address_nettest: %d failure(s)\n", g_failures);
  return 1;
}
