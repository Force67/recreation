#include "core/job_system.h"

namespace rec {

JobSystem::JobSystem(unsigned thread_count) {
  if (thread_count == 0) {
    unsigned hw = std::thread::hardware_concurrency();
    thread_count = hw > 2 ? hw - 1 : 1;
  }
  workers_.reserve(thread_count);
  for (unsigned i = 0; i < thread_count; ++i) {
    workers_.emplace_back([this] { WorkerLoop(); });
  }
}

JobSystem::~JobSystem() {
  {
    std::scoped_lock lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  for (auto& worker : workers_) worker.join();
}

void JobSystem::Submit(std::function<void()> job) {
  {
    std::scoped_lock lock(mutex_);
    queue_.push_back(std::move(job));
  }
  wake_.notify_one();
}

void JobSystem::WaitIdle() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [this] { return queue_.empty() && in_flight_.load() == 0; });
}

void JobSystem::WorkerLoop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty()) return;
      job = std::move(queue_.front());
      queue_.pop_front();
      in_flight_.fetch_add(1);
    }
    job();
    in_flight_.fetch_sub(1);
    idle_.notify_all();
  }
}

}  // namespace rec
