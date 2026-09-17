#ifndef RECREATION_MASTERLIST_ANNOUNCER_H_
#define RECREATION_MASTERLIST_ANNOUNCER_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "components/masterlist/masterlist_client.h"

namespace rx::masterlist {

// Keeps one hosted session listed for as long as it runs: announce, beat on
// the cadence the list asks for, retire on the way out. Owns a thread, so no
// masterlist call ever happens on the frame loop.
//
// A list that is down, slow or refusing is never fatal here: the session keeps
// running unlisted and the announcer keeps retrying with a backoff. Whatever
// went wrong is in status(), which is what a server operator needs to see.
class Announcer {
 public:
  Announcer() = default;
  ~Announcer();

  Announcer(const Announcer&) = delete;
  Announcer& operator=(const Announcer&) = delete;

  // `players` is called from the announcer thread on every beat, so it must be
  // safe to call from there (an atomic load of the session's client count).
  // Starting twice is a no-op; Stop first to re-announce with new details.
  void Start(const base::String& base_url,
             const ServerInfo& info,
             std::function<u32()> players);

  // Retires the entry and joins the thread. Called by the destructor too.
  void Stop();

  bool running() const { return running_; }
  // Announced, with a live token: the entry is in the browser right now.
  bool listed() const;
  // "ip:port" as the list saw it, empty until the first announce lands. This
  // is the address to hand a friend, not what the host thinks its IP is.
  base::String address() const;
  // The last thing that went wrong, empty while everything is working.
  base::String status() const;

 private:
  void Run(base::String base_url, ServerInfo info);
  // Sleeps up to `seconds`, waking early when Stop is called. False when it
  // was told to stop.
  bool Wait(u32 seconds);

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::function<u32()> players_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  base::String address_;
  base::String status_;
  bool listed_ = false;
};

}  // namespace rx::masterlist

#endif  // RECREATION_MASTERLIST_ANNOUNCER_H_
