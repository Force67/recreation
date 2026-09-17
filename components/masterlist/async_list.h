#ifndef RECREATION_MASTERLIST_ASYNC_LIST_H_
#define RECREATION_MASTERLIST_ASYNC_LIST_H_

#include <atomic>
#include <mutex>
#include <thread>

#include "components/masterlist/masterlist_client.h"

namespace rx::masterlist {

// One list query, run on a worker and picked up from the frame loop. The menu
// asks for servers when the player opens the Join screen and polls until the
// answer lands, so a list on the other side of the planet costs frames nothing.
//
// One query at a time: Start while a query is in flight is ignored rather than
// queued, because the only caller is a screen that is already showing "looking
// for servers" and wants the answer it asked for.
class AsyncList {
 public:
  AsyncList() = default;
  ~AsyncList();

  AsyncList(const AsyncList&) = delete;
  AsyncList& operator=(const AsyncList&) = delete;

  // False when a query was already running and this one was dropped.
  bool Start(const base::String& base_url, const ListQuery& query);

  // True between Start and the result being taken by Poll.
  bool pending() const { return pending_; }

  // Moves the finished result into `out` and returns true, exactly once per
  // query. False while the query is still running or when there is nothing to
  // collect.
  bool Poll(ListResult* out);

  // Bounds how long a query can run, and with it how long ~AsyncList can block
  // when the player quits mid-query.
  void set_timeout_ms(u32 ms) { timeout_ms_ = ms; }

 private:
  std::thread thread_;
  std::atomic<bool> pending_{false};
  u32 timeout_ms_ = 6000;

  std::mutex mutex_;
  ListResult result_;
  bool ready_ = false;
};

}  // namespace rx::masterlist

#endif  // RECREATION_MASTERLIST_ASYNC_LIST_H_
