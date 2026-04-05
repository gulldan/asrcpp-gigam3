#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <vector>

#include "asr/executor.h"

namespace asr {
namespace {

using namespace std::chrono_literals;

TEST(Executor, RunsSubmittedTasksAndBecomesIdle) {
  BoundedExecutor executor(2, 8);

  std::atomic<int>   sum{0};
  std::atomic<int>   completed{0};
  std::promise<void> done;
  auto               done_future = done.get_future();

  for (int i = 1; i <= 4; ++i) {
    ASSERT_TRUE(executor.try_submit([&sum, &completed, &done, i]() {
      sum.fetch_add(i, std::memory_order_relaxed);
      if (completed.fetch_add(1, std::memory_order_acq_rel) + 1 == 4) {
        done.set_value();
      }
    }));
  }

  ASSERT_EQ(done_future.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(executor.wait_for_idle(1s));
  EXPECT_EQ(sum.load(std::memory_order_relaxed), 10);
}

TEST(Executor, RejectsWhenQueueCapacityExceeded) {
  BoundedExecutor executor(1, 1);

  std::promise<void> started;
  auto               started_future = started.get_future();
  std::promise<void> unblock;
  auto               unblock_future = unblock.get_future().share();

  ASSERT_TRUE(executor.try_submit([&started, unblock_future]() {
    started.set_value();
    unblock_future.wait();
  }));
  ASSERT_EQ(started_future.wait_for(1s), std::future_status::ready);

  ASSERT_TRUE(executor.try_submit([]() {}));
  EXPECT_FALSE(executor.try_submit([]() {}));

  unblock.set_value();
  EXPECT_TRUE(executor.wait_for_idle(1s));
}

TEST(Executor, SerializedTaskQueuePreservesOrderAndBackpressure) {
  SerializedTaskQueue queue(1);
  std::vector<int>    started;

  ASSERT_TRUE(queue.push_or_start([&started]() {
    started.push_back(1);
    return true;
  }));
  EXPECT_TRUE(queue.in_flight());
  EXPECT_TRUE(queue.push_or_start([&started]() {
    started.push_back(2);
    return true;
  }));
  EXPECT_EQ(queue.pending(), 1U);
  EXPECT_FALSE(queue.push_or_start([&started]() {
    started.push_back(3);
    return true;
  }));

  queue.finish_current();
  ASSERT_TRUE(queue.maybe_start_next());
  EXPECT_EQ(started, (std::vector<int>{1, 2}));
  EXPECT_TRUE(queue.in_flight());
}

TEST(Executor, SerializedTaskQueueRetriesAfterTransientStartFailure) {
  SerializedTaskQueue queue(2);
  int                 attempts = 0;

  ASSERT_TRUE(queue.push_or_start([&attempts]() {
    ++attempts;
    return false;
  }));
  EXPECT_FALSE(queue.in_flight());
  EXPECT_EQ(queue.pending(), 1U);

  EXPECT_FALSE(queue.maybe_start_next());
  EXPECT_EQ(attempts, 2);

  queue.clear();
  attempts = 0;
  ASSERT_TRUE(queue.push_or_start([&attempts]() {
    ++attempts;
    return attempts >= 2;
  }));
  EXPECT_EQ(attempts, 1);
  EXPECT_FALSE(queue.in_flight());
  ASSERT_TRUE(queue.maybe_start_next());
  EXPECT_EQ(attempts, 2);
  EXPECT_TRUE(queue.in_flight());
}

TEST(Executor, SerializedTaskQueueStopRejectsFurtherWork) {
  SerializedTaskQueue queue(2);

  ASSERT_TRUE(queue.push_or_start([]() { return true; }));
  queue.stop();
  queue.finish_current();

  EXPECT_TRUE(queue.stopped());
  EXPECT_FALSE(queue.push_or_start([]() { return true; }));
  EXPECT_FALSE(queue.maybe_start_next());
}

}  // namespace
}  // namespace asr
