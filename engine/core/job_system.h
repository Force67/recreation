#ifndef RECREATION_CORE_JOB_SYSTEM_H_
#define RECREATION_CORE_JOB_SYSTEM_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace rec {

class JobSystem {
 public:
  explicit JobSystem(unsigned thread_count = 0);
  ~JobSystem();

  JobSystem(const JobSystem&) = delete;
  JobSystem& operator=(const JobSystem&) = delete;

  void Submit(std::function<void()> job);
  void WaitIdle();

  unsigned thread_count() const { return static_cast<unsigned>(workers_.size()); }

 private:
  void WorkerLoop();

  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> queue_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable idle_;
  std::atomic<unsigned> in_flight_{0};
  bool stop_ = false;
};

}  // namespace rec

#endif  // RECREATION_CORE_JOB_SYSTEM_H_
