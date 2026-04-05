#include "asr/executor.h"

#include <_stdio.h>

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>

namespace asr {

BoundedExecutor::BoundedExecutor(size_t worker_count, size_t queue_capacity)
    : queue_capacity_(queue_capacity) {
  if (worker_count == 0) {
    throw std::invalid_argument("BoundedExecutor worker_count must be positive");
  }
  if (queue_capacity_ == 0) {
    throw std::invalid_argument("BoundedExecutor queue_capacity must be positive");
  }

  workers_.reserve(worker_count);
  for (size_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back([this]() { worker_loop(); });
  }
}

BoundedExecutor::~BoundedExecutor() {
  shutdown();
}

bool BoundedExecutor::try_submit(Task task) {
  if (!task) {
    return false;
  }

  {
    std::lock_guard lock(mutex_);
    if (stopping_ || queue_.size() >= queue_capacity_) {
      return false;
    }
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
  return true;
}

void BoundedExecutor::shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  cv_.notify_all();
  idle_cv_.notify_all();

  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

bool BoundedExecutor::wait_for_idle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  return idle_cv_.wait_for(lock, timeout, [this]() { return queue_.empty() && in_flight_ == 0; });
}

size_t BoundedExecutor::queued() const {
  std::lock_guard lock(mutex_);
  return queue_.size();
}

size_t BoundedExecutor::in_flight() const {
  std::lock_guard lock(mutex_);
  return in_flight_;
}

size_t BoundedExecutor::capacity() const noexcept {
  return queue_capacity_;
}

void BoundedExecutor::worker_loop() {
  for (;;) {
    Task task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
      if (stopping_ && queue_.empty()) {
        return;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
      ++in_flight_;
    }

    try {
      task();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "BoundedExecutor task error: %s\n", e.what());
    } catch (...) {
      std::fputs("BoundedExecutor task error: unknown exception\n", stderr);
    }

    {
      std::lock_guard lock(mutex_);
      --in_flight_;
      if (queue_.empty() && in_flight_ == 0) {
        idle_cv_.notify_all();
      }
    }
  }
}

SerializedTaskQueue::SerializedTaskQueue(size_t max_pending) : max_pending_(max_pending) {}

bool SerializedTaskQueue::push_or_start(StartFn start) {
  std::lock_guard lock(mutex_);
  if (!start || stopped_) {
    return false;
  }
  if (in_flight_ || !pending_.empty()) {
    if (pending_.size() >= max_pending_) {
      return false;
    }
    pending_.push_back(std::move(start));
    return true;
  }
  if (start()) {
    in_flight_ = true;
    return true;
  }
  if (pending_.size() >= max_pending_) {
    return false;
  }
  pending_.push_back(std::move(start));
  return true;
}

void SerializedTaskQueue::finish_current() {
  std::lock_guard lock(mutex_);
  in_flight_ = false;
}

bool SerializedTaskQueue::maybe_start_next() {
  std::lock_guard lock(mutex_);
  if (stopped_ || in_flight_ || pending_.empty()) {
    return false;
  }
  if (!pending_.front()()) {
    return false;
  }
  pending_.pop_front();
  in_flight_ = true;
  return true;
}

void SerializedTaskQueue::stop(bool drop_pending) {
  std::lock_guard lock(mutex_);
  stopped_ = true;
  if (drop_pending) {
    pending_.clear();
  }
}

void SerializedTaskQueue::clear() {
  std::lock_guard lock(mutex_);
  pending_.clear();
}

bool SerializedTaskQueue::in_flight() const noexcept {
  std::lock_guard lock(mutex_);
  return in_flight_;
}

bool SerializedTaskQueue::stopped() const noexcept {
  std::lock_guard lock(mutex_);
  return stopped_;
}

size_t SerializedTaskQueue::pending() const noexcept {
  std::lock_guard lock(mutex_);
  return pending_.size();
}

}  // namespace asr
