#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "asr/logging.h"

namespace asr {
namespace {

TEST(Logging, RateLimiterAllowsFirstThenSuppressesBurst) {
  LogRateLimiter limiter(std::chrono::milliseconds(10));

  EXPECT_TRUE(limiter.allow());
  EXPECT_FALSE(limiter.allow());
  EXPECT_FALSE(limiter.allow());
}

TEST(Logging, RateLimiterAllowsAgainAfterInterval) {
  LogRateLimiter limiter(std::chrono::milliseconds(5));

  EXPECT_TRUE(limiter.allow());
  EXPECT_FALSE(limiter.allow());

  std::this_thread::sleep_for(std::chrono::milliseconds(8));
  EXPECT_TRUE(limiter.allow());
}

}  // namespace
}  // namespace asr
