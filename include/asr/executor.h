#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace asr {

class BoundedExecutor {
 public:
  using Task = std::function<void()>;

  BoundedExecutor(size_t worker_count, size_t queue_capacity);
  ~BoundedExecutor();

  BoundedExecutor(const BoundedExecutor&)            = delete;
  BoundedExecutor& operator=(const BoundedExecutor&) = delete;
  BoundedExecutor(BoundedExecutor&&)                 = delete;
  BoundedExecutor& operator=(BoundedExecutor&&)      = delete;

  bool try_submit(Task task);
  void shutdown();
  bool wait_for_idle(std::chrono::milliseconds timeout);

  [[nodiscard]] size_t queued() const;
  [[nodiscard]] size_t in_flight() const;
  [[nodiscard]] size_t capacity() const noexcept;

 private:
  void worker_loop();

  const size_t             queue_capacity_;
  mutable std::mutex       mutex_;
  std::condition_variable  cv_;
  std::condition_variable  idle_cv_;
  std::deque<Task>         queue_;
  std::vector<std::thread> workers_;
  size_t                   in_flight_ = 0;
  bool                     stopping_  = false;
};

class SerializedTaskQueue {
 public:
  using StartFn = std::function<bool()>;

  explicit SerializedTaskQueue(size_t max_pending);

  bool push_or_start(StartFn start);
  void finish_current();
  bool maybe_start_next();
  void stop(bool drop_pending = true);
  void clear();

  [[nodiscard]] bool   in_flight() const noexcept;
  [[nodiscard]] bool   stopped() const noexcept;
  [[nodiscard]] size_t pending() const noexcept;

 private:
  mutable std::mutex  mutex_;
  size_t              max_pending_ = 0;
  std::deque<StartFn> pending_;
  bool                in_flight_ = false;
  bool                stopped_   = false;
};

}  // namespace asr
