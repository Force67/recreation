// masterlisttest: the contract with the server list, checked without a network.
// The list is a service in another repo, so what matters here is that the bytes
// we send are the shape it parses, that the bytes it sends come back as
// listings, and that a hostile or broken answer is refused rather than half
// believed. The load-order digest is checked for the properties both sides rely
// on: order sensitive, case blind, separator safe.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "components/masterlist/announcer.h"
#include "components/masterlist/async_list.h"
#include "components/masterlist/json.h"
#include "components/masterlist/load_order_digest.h"
#include "components/masterlist/masterlist_client.h"

using namespace rx;
using namespace rx::masterlist;

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

bool Contains(const base::String& haystack, const char* needle) {
  return haystack.find(needle) != base::String::npos;
}

void TestJsonReader() {
  JsonDoc doc;
  CHECK(doc.Parse("{\"a\":1,\"b\":\"two\",\"c\":true,\"d\":null,\"e\":[1,2,3]}"));
  const u32 root = doc.root();
  CHECK(doc.MemberU32(root, "a") == 1u);
  CHECK(doc.MemberStr(root, "b") == base::String("two"));
  CHECK(doc.MemberBool(root, "c"));
  CHECK(doc.KindOf(doc.Member(root, "d")) == JsonDoc::Kind::kNull);
  CHECK(doc.Count(doc.Member(root, "e")) == 3u);
  CHECK(doc.Num(doc.Element(doc.Member(root, "e"), 2)) == 3.0);

  // An absent field answers with the fallback: a list that grows or drops a
  // field must not break a client of the other vintage.
  CHECK(doc.MemberU32(root, "nothing", 7u) == 7u);
  CHECK(doc.MemberStr(root, "nothing", "-") == base::String("-"));

  // Escapes, including a surrogate pair, come out as UTF-8.
  JsonDoc text;
  CHECK(text.Parse("{\"n\":\"a\\\"b\\\\c\\nd\\u00e9\\ud83d\\ude80\"}"));
  const base::String value = text.MemberStr(text.root(), "n");
  CHECK(Contains(value, "a\"b\\c\nd"));
  CHECK(value.size() == 13);  // 7 ascii + 2 for é + 4 for the rocket

  // A number JSON allows but u64 cannot hold answers the fallback rather than
  // casting an infinity (undefined: 0 on x86, saturated on aarch64).
  JsonDoc huge;
  CHECK(huge.Parse("{\"a\":1e999,\"b\":-5,\"c\":1e30}"));
  CHECK(huge.MemberU64(huge.root(), "a", 42) == 42u);
  CHECK(huge.MemberU32(huge.root(), "a", 7) == 7u);
  CHECK(huge.MemberU64(huge.root(), "b", 42) == 42u);
  CHECK(huge.MemberU32(huge.root(), "c", 7) == 7u);

  JsonDoc bad;
  CHECK(!bad.Parse(""));
  // An escaped NUL would leave a string whose size() and c_str() disagree.
  CHECK(!bad.Parse("{\"a\":\"ev\\u0000il\"}"));
  CHECK(!bad.Parse("{"));
  CHECK(!bad.Parse("{\"a\":}"));
  CHECK(!bad.Parse("{\"a\":1}trailing"));
  CHECK(!bad.Parse("{\"a\":\"unterminated}"));
  CHECK(!bad.Parse("{\"a\":\"\\ud83d\"}"));  // half a surrogate pair
  CHECK(!bad.Parse("[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]"));
}

void TestJsonWriter() {
  base::Vector<base::String> tags;
  tags.push_back("voice");
  tags.push_back("modded");

  JsonWriter writer;
  writer.Str("name", "Vince's \"Campaign\"\n");
  writer.Num("port", 29700);
  writer.Bool("passworded", false);
  writer.StrArray("tags", tags);
  const base::String body = writer.Finish();

  // What we write has to read back as what we meant, control characters and
  // quotes included.
  JsonDoc doc;
  CHECK(doc.Parse(body));
  CHECK(doc.MemberStr(doc.root(), "name") == base::String("Vince's \"Campaign\"\n"));
  CHECK(doc.MemberU32(doc.root(), "port") == 29700u);
  CHECK(!doc.MemberBool(doc.root(), "passworded", true));
  CHECK(doc.Count(doc.Member(doc.root(), "tags")) == 2u);
}

void TestQuoteRejectsBadUtf8() {
  // A server name comes off the command line and can hold any byte. One
  // Latin-1 byte used to make the whole announce a 400 the host then retried
  // forever, so ill-formed input is replaced rather than passed through.
  base::String latin1;
  latin1 += "Br";
  latin1.push_back(static_cast<char>(0xfc));  // 'ü' in Latin-1, not UTF-8
  latin1 += "nja";

  JsonWriter writer;
  writer.Str("name", latin1);
  const base::String body = writer.Finish();

  JsonDoc doc;
  CHECK(doc.Parse(body));  // and it is valid JSON, which is the point
  const base::String name = doc.MemberStr(doc.root(), "name");
  CHECK(name.find("Br") != base::String::npos);
  CHECK(name.find("nja") != base::String::npos);
  for (mem_size i = 0; i < name.size(); ++i)
    CHECK(static_cast<unsigned char>(name[i]) != 0xfc);

  // Real UTF-8 survives untouched, multi-byte and 4-byte alike.
  JsonWriter utf8;
  utf8.Str("name", "Brynja \xc3\xa9 \xf0\x9f\x9a\x80");
  JsonDoc round;
  CHECK(round.Parse(utf8.Finish()));
  CHECK(round.MemberStr(round.root(), "name") ==
        base::String("Brynja \xc3\xa9 \xf0\x9f\x9a\x80"));
}

void TestEncodeAnnounce() {
  ServerInfo info;
  info.port = 29700;
  info.name = "Whiterun Roleplay";
  info.gametype = "roleplay";
  info.domain = "skyrim";
  info.players = 12;
  info.max_players = 64;
  info.version = "0.1.0";
  info.plugins = "0123456789abcdef";
  info.plugin_count = 142;
  info.resources = 4;
  info.resources_bytes = 22u * 1024 * 1024;

  const base::String body = EncodeAnnounce(info);
  JsonDoc doc;
  CHECK(doc.Parse(body));
  const u32 root = doc.root();
  CHECK(doc.MemberU32(root, "port") == 29700u);
  CHECK(doc.MemberStr(root, "name") == base::String("Whiterun Roleplay"));
  CHECK(doc.MemberU32(root, "max_players") == 64u);
  CHECK(doc.MemberU64(root, "resources_bytes") == 22ull * 1024 * 1024);
  // The address is the list's to decide, never the host's to claim.
  CHECK(doc.Member(root, "address") == JsonDoc::kInvalid);
  CHECK(doc.Member(root, "ip") == JsonDoc::kInvalid);
}

void TestEncodeQuery() {
  ListQuery query;
  CHECK(EncodeListQuery(query).empty());

  query.domain = "skyrim";
  query.text = "vince & co";
  query.has_slots = true;
  query.limit = 50;
  const base::String encoded = EncodeListQuery(query);
  CHECK(Contains(encoded, "?domain=skyrim"));
  CHECK(Contains(encoded, "&has_slots=1"));
  CHECK(Contains(encoded, "&limit=50"));
  // A name with an ampersand must not become a second filter.
  CHECK(Contains(encoded, "q=vince%20%26%20co"));
  CHECK(!Contains(encoded, "no_password"));
}

void TestDecodeAnnounce() {
  AnnounceResult result;
  CHECK(DecodeAnnounce(
      "{\"address\":\"203.0.113.9:29700\",\"token\":\"abc\",\"heartbeat_secs\":30,"
      "\"entry_ttl_secs\":120}",
      &result));
  CHECK(result.ok);
  CHECK(result.address == base::String("203.0.113.9:29700"));
  CHECK(result.token == base::String("abc"));
  CHECK(result.heartbeat_secs == 30u);

  // A reply with no token cannot be kept alive, so it is not an announce.
  AnnounceResult tokenless;
  CHECK(!DecodeAnnounce("{\"address\":\"203.0.113.9:29700\"}", &tokenless));
  CHECK(!tokenless.ok);

  // An absurd cadence is clamped rather than obeyed.
  AnnounceResult silly;
  CHECK(DecodeAnnounce("{\"token\":\"t\",\"heartbeat_secs\":0}", &silly));
  CHECK(silly.heartbeat_secs == 5u);
  // A list that states no TTL (0) only gets the absolute cap applied.
  CHECK(DecodeAnnounce(
      "{\"token\":\"t\",\"heartbeat_secs\":99999,\"entry_ttl_secs\":0}", &silly));
  CHECK(silly.heartbeat_secs == 600u);
  // With the default TTL in play, the TTL is the tighter of the two bounds.
  CHECK(DecodeAnnounce("{\"token\":\"t\",\"heartbeat_secs\":99999}", &silly));
  CHECK(silly.heartbeat_secs == 40u);

  // Cadence and TTL are independent knobs on the list, so a deployment can ask
  // for a beat slower than the TTL it sweeps on. Obeying that puts the entry
  // out of the browser between beats while the host believes it is listed.
  AnnounceResult slow;
  CHECK(DecodeAnnounce(
      "{\"token\":\"t\",\"heartbeat_secs\":300,\"entry_ttl_secs\":120}", &slow));
  CHECK(slow.heartbeat_secs == 40u);  // a third of the ttl, room for two misses
  // A cadence that already fits the TTL is left alone.
  AnnounceResult sane;
  CHECK(DecodeAnnounce(
      "{\"token\":\"t\",\"heartbeat_secs\":30,\"entry_ttl_secs\":120}", &sane));
  CHECK(sane.heartbeat_secs == 30u);
}

void TestDecodeList() {
  const base::String body =
      "{\"total\":2,\"servers\":["
      "{\"address\":\"203.0.113.9:29700\",\"name\":\"Vince's Campaign\","
      "\"gametype\":\"campaign-coop\",\"domain\":\"skyrim\",\"mode_id\":\"\","
      "\"players\":2,\"max_players\":4,\"passworded\":false,\"tags\":[\"voice\"],"
      "\"version\":\"0.1.0\",\"plugins\":\"deadbeef\",\"plugin_count\":139,"
      "\"resources\":4,\"resources_bytes\":23068672,\"age_secs\":3,"
      "\"uptime_secs\":2460,\"verified\":true},"
      "{\"address\":\"198.51.100.4:29701\",\"name\":\"Commonwealth RP\","
      "\"domain\":\"fallout4\",\"players\":88,\"max_players\":128}]}";

  ListResult result;
  CHECK(DecodeList(body, &result));
  CHECK(result.ok);
  CHECK(result.total == 2u);
  CHECK(result.servers.size() == 2);
  CHECK(result.servers[0].name == base::String("Vince's Campaign"));
  CHECK(result.servers[0].verified);
  CHECK(result.servers[0].plugin_count == 139u);
  CHECK(result.servers[0].tags.size() == 1);
  CHECK(result.servers[0].has_slots());
  // The second entry omits most fields; what is missing takes its default
  // rather than failing the whole page.
  CHECK(result.servers[1].domain == base::String("fallout4"));
  CHECK(result.servers[1].players == 88u);
  CHECK(result.servers[1].has_slots());
  CHECK(!result.servers[1].verified);
  CHECK(result.servers[1].version.empty());
  CHECK(result.servers[1].tags.empty());

  // An entry with nothing to dial is dropped, the rest of the page survives.
  ListResult partial;
  CHECK(DecodeList("{\"total\":1,\"servers\":[{\"name\":\"nowhere\"}]}", &partial));
  CHECK(partial.servers.empty());

  ListResult broken;
  CHECK(!DecodeList("{\"total\":1}", &broken));
  CHECK(!DecodeList("not json", &broken));
  CHECK(!DecodeList("{\"servers\":\"soon\"}", &broken));
}

void TestDecodeError() {
  CHECK(DecodeError(429, "{\"error\":\"rate_limited\",\"detail\":\"too many announces\"}") ==
        base::String("too many announces"));
  CHECK(DecodeError(503, "{\"error\":\"list_full\"}") == base::String("list_full"));
  // An answer that is not ours still has to say something useful.
  CHECK(DecodeError(502, "<html>bad gateway</html>") == base::String("HTTP 502"));
}

void TestClientRefusals() {
  // A client with an unusable URL fails every call locally, without a socket,
  // and says why.
  Client client("totally not a url");
  CHECK(!client.valid());

  ServerInfo info;
  info.port = 29700;
  info.name = "x";
  info.domain = "skyrim";
  const AnnounceResult announced = client.Announce(info);
  CHECK(!announced.ok);
  CHECK(!announced.error.empty());

  base::String error;
  CHECK(!client.Heartbeat("token", 1, &error));
  CHECK(!error.empty());
  CHECK(!client.Retire("token", &error));

  const ListResult listed = client.List(ListQuery{});
  CHECK(!listed.ok);
  CHECK(!listed.error.empty());

  // A trailing slash on the base URL must not double up against "/v1/...".
  Client trailing("http://list.example:8477/");
  CHECK(trailing.valid());
  CHECK(trailing.base_url() == base::String("http://list.example:8477"));
}

void TestAsyncListFailsCleanly() {
  // No network here: the query fails, but it has to fail on the worker and
  // arrive through Poll exactly once, which is the part the menu depends on.
  AsyncList async;
  async.set_timeout_ms(500);
  CHECK(async.Start("not a url", ListQuery{}));

  ListResult result;
  for (int i = 0; i < 200 && !async.Poll(&result); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(!result.ok);
  CHECK(!result.error.empty());
  CHECK(!async.Poll(&result));  // taken once, and once only
}

void TestLoadOrderDigest() {
  base::Vector<base::String> a;
  a.push_back("Skyrim.esm");
  a.push_back("Update.esm");

  base::Vector<base::String> same_case;
  same_case.push_back("skyrim.esm");
  same_case.push_back("UPDATE.ESM");

  base::Vector<base::String> reordered;
  reordered.push_back("Update.esm");
  reordered.push_back("Skyrim.esm");

  base::Vector<base::String> extra = a;
  extra.push_back("Dawnguard.esm");

  // Case cannot matter (Windows and Linux disagree), order must.
  CHECK(LoadOrderDigest(a) == LoadOrderDigest(same_case));
  CHECK(LoadOrderDigest(a) != LoadOrderDigest(reordered));
  CHECK(LoadOrderDigest(a) != LoadOrderDigest(extra));
  CHECK(LoadOrderDigest(a).size() == 16);
  const base::Vector<base::String> nothing;
  CHECK(LoadOrderDigest(nothing) != LoadOrderDigest(a));

  // The separator is what keeps a split from colliding with a join.
  base::Vector<base::String> split;
  split.push_back("Skyrim");
  split.push_back(".esmUpdate.esm");
  CHECK(LoadOrderDigest(a) != LoadOrderDigest(split));
}

// A listener that accepts and then says nothing, which is how a list that has
// gone away behaves: the socket connects, the request goes out, and no answer
// ever comes.
class BlackHole {
 public:
  BlackHole() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    const int on = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listener_, 8);
    socklen_t len = sizeof(addr);
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] {
      while (!done_.load()) {
        const int client = ::accept(listener_, nullptr, nullptr);
        if (client < 0)
          break;
        held_.push_back(client);  // hold it open, answer nothing
      }
    });
  }

  ~BlackHole() {
    done_ = true;
    ::shutdown(listener_, SHUT_RDWR);
    ::close(listener_);
    if (thread_.joinable())
      thread_.join();
    for (int held : held_)
      ::close(held);
  }

  base::String url() const {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "http://127.0.0.1:%u", unsigned(port_));
    return base::String(buffer);
  }

 private:
  int listener_ = -1;
  u16 port_ = 0;
  std::atomic<bool> done_{false};
  std::vector<int> held_;
  std::thread thread_;
};

void TestStopIsPromptAgainstADeadList() {
  // The whole point of the cancel flag: quitting the game must not wait out an
  // announce that will never be answered. Stop() used to join a thread parked
  // in a socket read, which measured 7.7 seconds against exactly this server.
  BlackHole list;
  Announcer announcer;
  ServerInfo info;
  info.port = 29788;
  info.name = "stop test";
  info.domain = "skyrim";
  info.max_players = 4;
  announcer.Start(list.url(), info, [] { return 0u; });

  // Let it get as far as blocking on the answer.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  const auto began = std::chrono::steady_clock::now();
  announcer.Stop();
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - began)
                        .count();
  CHECK(!announcer.running());
  CHECK(took < 1500);
  if (took >= 1500)
    std::printf("  Stop() took %lld ms\n", static_cast<long long>(took));
}

void TestAsyncListDropsAQueryOnTeardown() {
  BlackHole list;
  const auto began = std::chrono::steady_clock::now();
  {
    AsyncList async;
    async.set_timeout_ms(30000);  // long enough that only the cancel ends it
    CHECK(async.Start(list.url(), ListQuery{}));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }  // the destructor raises the flag and joins
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - began)
                        .count();
  CHECK(took < 2000);
  if (took >= 2000)
    std::printf("  ~AsyncList took %lld ms\n", static_cast<long long>(took));
}

// Off by default: this one needs a masterlist to talk to. Point it at a running
// instance to prove the whole contract end to end, which is the only way to
// catch the service and the game drifting apart:
//   RXML_PUBLIC_ONLY=0 ./recreation-masterlist &
//   RX_MASTERLIST_LIVE=http://127.0.0.1:8477 ./masterlisttest
void TestLive() {
  const char* base_url = std::getenv("RX_MASTERLIST_LIVE");
  if (base_url == nullptr || *base_url == '\0')
    return;
  std::printf("live masterlist at %s:\n", base_url);

  Client client(base_url);
  CHECK(client.valid());

  ServerInfo info;
  info.port = 29788;  // a port nothing is listening on; the list only stores it
  info.name = "masterlisttest";
  info.gametype = "campaign-coop";
  info.domain = "skyrim";
  info.version = "0.1.0";
  info.players = 1;
  info.max_players = 4;
  info.plugin_count = 2;
  info.tags.push_back("test");

  base::Vector<base::String> load_order;
  load_order.push_back("Skyrim.esm");
  load_order.push_back("Update.esm");
  info.plugins = LoadOrderDigest(load_order);

  const AnnounceResult announced = client.Announce(info);
  std::printf("  announce: ok=%d address=%s error=%s\n", int(announced.ok),
              announced.address.c_str(), announced.error.c_str());
  CHECK(announced.ok);
  if (!announced.ok)
    return;

  ListQuery query;
  query.domain = "skyrim";
  const ListResult listed = client.List(query);
  std::printf("  list: ok=%d total=%u error=%s\n", int(listed.ok), listed.total,
              listed.error.c_str());
  CHECK(listed.ok);

  bool found = false;
  for (mem_size i = 0; i < listed.servers.size(); ++i) {
    if (listed.servers[i].address != announced.address)
      continue;
    found = true;
    // Every field has to survive the round trip through the service.
    CHECK(listed.servers[i].name == info.name);
    CHECK(listed.servers[i].max_players == 4u);
    CHECK(listed.servers[i].plugins == info.plugins);
    CHECK(listed.servers[i].tags.size() == 1);
  }
  CHECK(found);

  base::String error;
  CHECK(client.Heartbeat(announced.token, 2, &error));
  CHECK(client.Retire(announced.token, &error));

  // Retired means gone now, not gone when the entry ages out.
  const ListResult after = client.List(query);
  bool still_there = false;
  for (mem_size i = 0; i < after.servers.size(); ++i)
    if (after.servers[i].address == announced.address)
      still_there = true;
  CHECK(!still_there);
}

}  // namespace

int main() {
  std::puts("masterlist:");
  TestJsonReader();
  TestJsonWriter();
  TestQuoteRejectsBadUtf8();
  TestEncodeAnnounce();
  TestEncodeQuery();
  TestDecodeAnnounce();
  TestDecodeList();
  TestDecodeError();
  TestClientRefusals();
  TestAsyncListFailsCleanly();
  TestLoadOrderDigest();
  TestStopIsPromptAgainstADeadList();
  TestAsyncListDropsAQueryOnTeardown();
  TestLive();
  if (g_failures == 0) {
    std::puts("masterlisttest: all passed");
    return 0;
  }
  std::printf("masterlisttest: %d failure(s)\n", g_failures);
  return 1;
}
