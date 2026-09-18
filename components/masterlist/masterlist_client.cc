#include "components/masterlist/masterlist_client.h"

#include <cstdio>

#include "components/masterlist/json.h"
#include "http/http.h"

namespace rx::masterlist {
namespace {

base::String Decimal(u64 value) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return base::String(buffer);
}

// Percent-encodes a query value. Only the unreserved set survives untouched,
// which is more conservative than it needs to be and keeps a server name with
// a '&' in it from turning into a second filter.
base::String UrlEncode(const base::String& value) {
  static const char* kHex = "0123456789ABCDEF";
  base::String out;
  for (mem_size i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                            c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0f]);
    }
  }
  return out;
}

void AppendFilter(base::String* query, const char* key, const base::String& value) {
  if (value.empty())
    return;
  *query += query->empty() ? "?" : "&";
  *query += key;
  *query += "=";
  *query += UrlEncode(value);
}

void AppendFlag(base::String* query, const char* key, bool value) {
  if (!value)
    return;
  *query += query->empty() ? "?" : "&";
  *query += key;
  *query += "=1";
}

void AppendCount(base::String* query, const char* key, u32 value) {
  if (value == 0)
    return;
  *query += query->empty() ? "?" : "&";
  *query += key;
  *query += "=";
  *query += Decimal(value);
}

ServerEntry DecodeEntry(const JsonDoc& doc, u32 node) {
  ServerEntry entry;
  entry.address = doc.MemberStr(node, "address");
  entry.name = doc.MemberStr(node, "name");
  entry.gametype = doc.MemberStr(node, "gametype");
  entry.domain = doc.MemberStr(node, "domain");
  entry.mode_id = doc.MemberStr(node, "mode_id");
  entry.version = doc.MemberStr(node, "version");
  entry.plugins = doc.MemberStr(node, "plugins");
  entry.plugin_count = doc.MemberU32(node, "plugin_count");
  entry.players = doc.MemberU32(node, "players");
  entry.max_players = doc.MemberU32(node, "max_players");
  entry.resources = doc.MemberU32(node, "resources");
  entry.resources_bytes = doc.MemberU64(node, "resources_bytes");
  entry.age_secs = doc.MemberU64(node, "age_secs");
  entry.uptime_secs = doc.MemberU64(node, "uptime_secs");
  entry.passworded = doc.MemberBool(node, "passworded");
  entry.verified = doc.MemberBool(node, "verified");

  const u32 tags = doc.Member(node, "tags");
  for (u32 i = 0; i < doc.Count(tags); ++i)
    entry.tags.push_back(doc.Str(doc.Element(tags, i)));
  return entry;
}

}  // namespace

base::String EncodeAnnounce(const ServerInfo& info) {
  JsonWriter writer;
  writer.Num("port", info.port);
  writer.Str("name", info.name);
  writer.Str("gametype", info.gametype);
  writer.Str("domain", info.domain);
  writer.Str("mode_id", info.mode_id);
  writer.Num("players", info.players);
  writer.Num("max_players", info.max_players);
  writer.Bool("passworded", info.passworded);
  writer.StrArray("tags", info.tags);
  writer.Str("version", info.version);
  writer.Str("plugins", info.plugins);
  writer.Num("plugin_count", info.plugin_count);
  writer.Num("resources", info.resources);
  writer.Num("resources_bytes", info.resources_bytes);
  return writer.Finish();
}

base::String EncodeListQuery(const ListQuery& query) {
  base::String out;
  AppendFilter(&out, "domain", query.domain);
  AppendFilter(&out, "gametype", query.gametype);
  AppendFilter(&out, "mode_id", query.mode_id);
  AppendFilter(&out, "q", query.text);
  AppendFlag(&out, "has_slots", query.has_slots);
  AppendFlag(&out, "no_password", query.no_password);
  AppendFlag(&out, "verified_only", query.verified_only);
  AppendCount(&out, "limit", query.limit);
  AppendCount(&out, "offset", query.offset);
  return out;
}

bool DecodeAnnounce(const base::String& body, AnnounceResult* out) {
  JsonDoc doc;
  if (!doc.Parse(body))
    return false;
  const u32 root = doc.root();
  out->address = doc.MemberStr(root, "address");
  out->token = doc.MemberStr(root, "token");
  out->heartbeat_secs = doc.MemberU32(root, "heartbeat_secs", 30);
  out->entry_ttl_secs = doc.MemberU32(root, "entry_ttl_secs", 120);
  // A reply without a token is not an announce we can keep alive, whatever
  // else it contains.
  if (out->token.empty())
    return false;
  // A list that asks for a beat every second, or once an hour, is misconfigured
  // or hostile; clamp rather than trust it.
  if (out->heartbeat_secs < 5)
    out->heartbeat_secs = 5;
  if (out->heartbeat_secs > 600)
    out->heartbeat_secs = 600;
  // The cadence and the TTL are independent knobs on the list, so a deployment
  // can ask for a beat slower than the TTL it sweeps on. Obeying that literally
  // means the entry expires between beats and the host sits out of the browser
  // while believing it is listed. A third of the TTL leaves room for two lost
  // beats, which is also what the re-announce path needs.
  if (out->entry_ttl_secs > 0 && out->heartbeat_secs > out->entry_ttl_secs / 3) {
    const u32 third = out->entry_ttl_secs / 3;
    out->heartbeat_secs = third < 5 ? 5 : third;
  }
  out->ok = true;
  return true;
}

bool DecodeList(const base::String& body, ListResult* out) {
  JsonDoc doc;
  if (!doc.Parse(body))
    return false;
  const u32 root = doc.root();
  const u32 servers = doc.Member(root, "servers");
  if (doc.KindOf(servers) != JsonDoc::Kind::kArray)
    return false;
  out->total = doc.MemberU32(root, "total");
  for (u32 i = 0; i < doc.Count(servers); ++i) {
    ServerEntry entry = DecodeEntry(doc, doc.Element(servers, i));
    // An entry with nothing to dial is a bug on the other side; dropping it
    // beats putting an unjoinable row in the browser.
    if (!entry.address.empty())
      out->servers.push_back(std::move(entry));
  }
  out->ok = true;
  return true;
}

base::String DecodeError(u16 status, const base::String& body) {
  JsonDoc doc;
  if (doc.Parse(body)) {
    const base::String detail = doc.MemberStr(doc.root(), "detail");
    if (!detail.empty())
      return detail;
    const base::String error = doc.MemberStr(doc.root(), "error");
    if (!error.empty())
      return error;
  }
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "HTTP %u", static_cast<unsigned>(status));
  return base::String(buffer);
}

Client::Client(const base::String& base_url) : base_url_(base_url) {
  // A trailing slash would double up against the "/v1/..." paths below.
  while (!base_url_.empty() && base_url_[base_url_.size() - 1] == '/')
    base_url_ = base_url_.substr(0, base_url_.size() - 1);
  http::Url parsed;
  valid_ = !base_url_.empty() && http::Url::Parse(base_url_, &parsed);
}

base::String Client::Endpoint(const char* path) const {
  base::String out = base_url_;
  out += path;
  return out;
}

void Client::ApplyLimits(http::Request* request, u32 idle_ms) const {
  request->timeout_ms = idle_ms;
  request->total_timeout_ms = total_timeout_ms_;
  request->cancel = cancel_;
}

AnnounceResult Client::Announce(const ServerInfo& info) {
  AnnounceResult result;
  if (!valid_) {
    result.error = base::String("not a masterlist url: ") + base_url_;
    return result;
  }

  http::Request request;
  request.method = "POST";
  request.url = Endpoint("/v1/servers");
  request.body = EncodeAnnounce(info);
  request.content_type = "application/json";
  ApplyLimits(&request, timeout_ms_);
  // 301/302/303 are followed as GET, so a masterlist url that redirects (http
  // to https, say) would turn this POST into a list query that answers 200 with
  // no token. Refusing the redirect reports the url instead of the symptom.
  request.max_redirects = 0;

  const http::Response response = http::Fetch(request);
  if (response.status == 0) {
    result.error = response.error;
    result.cancelled = response.cancelled;
    return result;
  }
  if (!response.ok()) {
    result.error = DecodeError(response.status, response.body);
    return result;
  }
  if (!DecodeAnnounce(response.body, &result)) {
    result.ok = false;
    result.error = "the list answered an announce we cannot read";
  }
  return result;
}

bool Client::Heartbeat(const base::String& token,
                       u32 players,
                       base::String* error,
                       bool* token_rejected) {
  if (token_rejected != nullptr)
    *token_rejected = false;
  if (!valid_) {
    *error = base::String("not a masterlist url: ") + base_url_;
    return false;
  }

  JsonWriter writer;
  writer.Str("token", token);
  writer.Num("players", players);

  http::Request request;
  request.method = "POST";
  request.url = Endpoint("/v1/servers/heartbeat");
  request.body = writer.Finish();
  request.content_type = "application/json";
  ApplyLimits(&request, timeout_ms_);
  request.max_redirects = 0;  // a redirected POST becomes a GET; see Announce

  const http::Response response = http::Fetch(request);
  if (response.status == 0) {
    *error = response.error;
    return false;
  }
  if (!response.ok()) {
    *error = DecodeError(response.status, response.body);
    // 401 is the list saying it has never heard of this token: a restarted
    // list, or a slot that timed out. Only a fresh announce recovers it.
    if (response.status == 401 && token_rejected != nullptr)
      *token_rejected = true;
    return false;
  }
  error->clear();
  return true;
}

bool Client::Retire(const base::String& token, base::String* error) {
  if (!valid_) {
    *error = base::String("not a masterlist url: ") + base_url_;
    return false;
  }

  JsonWriter writer;
  writer.Str("token", token);

  http::Request request;
  request.method = "POST";
  request.url = Endpoint("/v1/servers/retire");
  request.body = writer.Finish();
  request.content_type = "application/json";
  request.max_redirects = 0;  // a redirected POST becomes a GET; see Announce
  // Retire runs on the way out, so it waits a shorter time than the rest: a
  // list that is down must not hold up the shutdown. It deliberately does NOT
  // take the cancel flag, which by then is already raised; the deadline is what
  // bounds it.
  ApplyLimits(&request, timeout_ms_ < 3000 ? timeout_ms_ : 3000);
  request.cancel = nullptr;
  request.total_timeout_ms = 3000;

  const http::Response response = http::Fetch(request);
  if (response.status == 0) {
    *error = response.error;
    return false;
  }
  if (!response.ok()) {
    *error = DecodeError(response.status, response.body);
    return false;
  }
  error->clear();
  return true;
}

ListResult Client::List(const ListQuery& query) {
  ListResult result;
  if (!valid_) {
    result.error = base::String("not a masterlist url: ") + base_url_;
    return result;
  }

  base::String url = Endpoint("/v1/servers");
  url += EncodeListQuery(query);

  http::Request request;
  request.url = url;
  ApplyLimits(&request, timeout_ms_);
  // A full page of 500 entries is a few hundred KB; anything past a megabyte
  // is not this endpoint answering.
  request.max_body_bytes = 4 * 1024 * 1024;

  const http::Response response = http::Fetch(request);
  if (response.status == 0) {
    result.error = response.error;
    result.cancelled = response.cancelled;
    return result;
  }
  if (!response.ok()) {
    result.error = DecodeError(response.status, response.body);
    return result;
  }
  if (!DecodeList(response.body, &result)) {
    result.ok = false;
    result.error = "the list answered with something that is not a server list";
  }
  return result;
}

}  // namespace rx::masterlist
