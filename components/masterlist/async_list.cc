#include "components/masterlist/async_list.h"

#include <utility>

namespace rx::masterlist {

AsyncList::~AsyncList() {
  // Tell the query to give up before joining it. The worker holds no lock while
  // it blocks on the socket, and the client checks this flag about four times a
  // second, so shutdown costs a wake tick rather than the query's timeout.
  abandon_ = true;
  if (thread_.joinable())
    thread_.join();
}

bool AsyncList::Start(const base::String& base_url, const ListQuery& query) {
  if (pending_)
    return false;
  if (thread_.joinable())
    thread_.join();  // the previous query is finished; this only reaps it
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result_ = ListResult{};
    ready_ = false;
  }
  pending_ = true;
  const u32 timeout_ms = timeout_ms_;
  thread_ = std::thread([this, base_url, query, timeout_ms] {
    Client client(base_url);
    client.set_timeout_ms(timeout_ms);
    client.set_total_timeout_ms(timeout_ms * 3);
    client.set_cancel(&abandon_);
    ListResult result = client.List(query);
    std::lock_guard<std::mutex> lock(mutex_);
    result_ = std::move(result);
    // Clear pending under the same lock that publishes the result, and before
    // it: outside, a Poll that took the answer could still see pending() true
    // and the caller's next Start would be refused for nothing.
    pending_ = false;
    ready_ = true;
  });
  return true;
}

bool AsyncList::Poll(ListResult* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_)
    return false;
  *out = std::move(result_);
  result_ = ListResult{};
  ready_ = false;
  return true;
}

}  // namespace rx::masterlist
