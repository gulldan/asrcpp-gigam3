#pragma once

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>

namespace asr {

class LogRateLimiter {
 public:
  explicit LogRateLimiter(std::chrono::milliseconds interval)
      : interval_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count()) {}

  [[nodiscard]] bool allow() {
    if (interval_ns_ <= 0) {
      return true;
    }

    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();

    int64_t next_allowed = next_allowed_ns_.load(std::memory_order_relaxed);
    while (now >= next_allowed) {
      if (next_allowed_ns_.compare_exchange_weak(next_allowed, now + interval_ns_, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
        return true;
      }
    }

    return false;
  }

 private:
  int64_t              interval_ns_;
  std::atomic<int64_t> next_allowed_ns_{0};
};

}  // namespace asr

#define ASR_LOG_RATELIMITED(level_name, interval_ms, ...)                                               \
  do {                                                                                                  \
    static ::asr::LogRateLimiter asr_log_rate_limiter_instance{std::chrono::milliseconds(interval_ms)}; \
    if (asr_log_rate_limiter_instance.allow()) {                                                        \
      spdlog::level_name(__VA_ARGS__);                                                                  \
    }                                                                                                   \
  } while (0)

#define ASR_LOG_DEBUG_EVERY(interval_ms, ...) ASR_LOG_RATELIMITED(debug, interval_ms, __VA_ARGS__)
#define ASR_LOG_INFO_EVERY(interval_ms, ...) ASR_LOG_RATELIMITED(info, interval_ms, __VA_ARGS__)
#define ASR_LOG_WARN_EVERY(interval_ms, ...) ASR_LOG_RATELIMITED(warn, interval_ms, __VA_ARGS__)
