#include "components/masterlist/announcer.h"

#include "core/log.h"

namespace rx::masterlist {
namespace {

// Backoff between failed announces: a list that is down should not be hammered
// by every server that wanted to be on it.
constexpr u32 kFirstRetrySecs = 5;
constexpr u32 kMaxRetrySecs = 60;
// Beats that may fail before the token is assumed dead and the whole announce
// is made again. One missed beat is a hiccup; two in a row is a restarted list
// that has never heard of us.
constexpr u32 kMissedBeatsBeforeReannounce = 2;

}  // namespace

Announcer::~Announcer() {
  Stop();
}

void Announcer::Start(const base::String& base_url,
                      const ServerInfo& info,
                      std::function<u32()> players) {
  if (running_)
    return;
  players_ = std::move(players);
  stop_ = false;
  running_ = true;
  thread_ = std::thread(&Announcer::Run, this, base_url, info);
}

void Announcer::Stop() {
  if (!running_ && !thread_.joinable())
    return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable())
    thread_.join();
  running_ = false;
}

bool Announcer::listed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return listed_;
}

base::String Announcer::address() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return address_;
}

base::String Announcer::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

bool Announcer::Wait(u32 seconds) {
  std::unique_lock<std::mutex> lock(mutex_);
  wake_.wait_for(lock, std::chrono::seconds(seconds), [this] { return stop_.load(); });
  return !stop_;
}

void Announcer::Run(base::String base_url, ServerInfo info) {
  Client client(base_url);
  // Stop() joins this thread, so whatever a call is waiting on is what the
  // player waits on when they quit. The stop flag goes to the client: a call in
  // flight is abandoned within a wake tick instead of running to its timeout.
  client.set_timeout_ms(4000);
  client.set_total_timeout_ms(15000);
  client.set_cancel(&stop_);
  base::String token;
  u32 heartbeat_secs = 30;
  u32 retry_secs = kFirstRetrySecs;
  u32 missed_beats = 0;

  while (!stop_) {
    if (token.empty()) {
      if (players_)
        info.players = players_();
      const AnnounceResult result = client.Announce(info);
      if (result.ok) {
        token = result.token;
        heartbeat_secs = result.heartbeat_secs;
        missed_beats = 0;
        retry_secs = kFirstRetrySecs;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          address_ = result.address;
          status_.clear();
          listed_ = true;
        }
        RX_INFO("masterlist: listed as {} (beat every {}s)", result.address.c_str(),
                heartbeat_secs);
      } else {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          status_ = result.error;
          listed_ = false;
        }
        // A cancel is this thread being told to stop, not the list failing.
        // Leave without the warning and without the backoff wait.
        if (result.cancelled)
          break;
        RX_WARN("masterlist: announce failed ({}), retrying in {}s", result.error.c_str(),
                retry_secs);
        if (!Wait(retry_secs))
          break;
        retry_secs = retry_secs * 2 > kMaxRetrySecs ? kMaxRetrySecs : retry_secs * 2;
        continue;
      }
    }

    if (!Wait(heartbeat_secs))
      break;

    const u32 players = players_ ? players_() : info.players;
    base::String error;
    bool token_rejected = false;
    if (client.Heartbeat(token, players, &error, &token_rejected)) {
      missed_beats = 0;
      std::lock_guard<std::mutex> lock(mutex_);
      status_.clear();
      listed_ = true;
      continue;
    }

    ++missed_beats;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_ = error;
    }
    // A rejected token is definitive, so it costs no further beats: the list has
    // never heard of us and only a fresh announce puts us back. Anything else
    // gets the benefit of the doubt until the second failure, because one lost
    // packet is not a lost slot.
    if (token_rejected || missed_beats >= kMissedBeatsBeforeReannounce) {
      RX_WARN("masterlist: {} ({}), announcing again",
              token_rejected ? "the list rejected our token" : "beats failed",
              error.c_str());
      token.clear();
      std::lock_guard<std::mutex> lock(mutex_);
      listed_ = false;
    }
  }

  if (!token.empty()) {
    // Retire on the way out so the entry goes immediately instead of aging out
    // of the browser over the next two minutes.
    base::String error;
    if (!client.Retire(token, &error))
      RX_WARN("masterlist: retire failed ({}), the entry will age out", error.c_str());
  }
  std::lock_guard<std::mutex> lock(mutex_);
  listed_ = false;
}

}  // namespace rx::masterlist
