/**
 * @file tests/unit/test_thread_safe.cpp
 * @brief Tests for thread-safe utility types.
 */

// standard includes
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <type_traits>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/thread_safe.h"

static_assert(!std::is_convertible_v<std::uint32_t, safe::queue_t<int>>);

TEST(ThreadSafeQueue, RejectsNewestItemAtCapacity) {
  safe::queue_t<int> queue {1, safe::queue_t<int>::overflow_policy_e::reject};

  ASSERT_TRUE(queue.raise(1));
  EXPECT_FALSE(queue.raise(2));

  const auto value = queue.pop();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 1);
}

TEST(ThreadSafeQueue, DropsQueuedItemsByDefaultAtCapacity) {
  safe::queue_t<int> queue {2};

  ASSERT_TRUE(queue.raise(1));
  ASSERT_TRUE(queue.raise(2));
  ASSERT_TRUE(queue.raise(3));

  const auto value = queue.pop();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 3);
}

TEST(ThreadSafeQueue, RejectsItemsAfterStop) {
  safe::queue_t<int> queue;

  queue.stop();

  EXPECT_FALSE(queue.raise(1));
}

TEST(ThreadSafeEvent, StaleExpiryAndClearLeaveReplacementLaunchUntouched) {
  safe::event_t<std::shared_ptr<std::uint32_t>> event;
  event.raise(std::make_shared<std::uint32_t>(101));
  ASSERT_TRUE(event.try_pop());
  const auto replacement = std::make_shared<std::uint32_t>(202);
  event.raise(replacement);

  const auto old_launch = [](const auto &pending) {
    return *pending == 101;
  };
  // Both the queued old expiry and the old control connection use this primitive.
  EXPECT_FALSE(event.try_pop_if(old_launch));
  EXPECT_FALSE(event.try_pop_if(old_launch));
  EXPECT_EQ(event.try_pop(), replacement);
}

TEST(ThreadSafeEvent, MatchingIdentityConsumesExactlyOneEvent) {
  safe::event_t<std::shared_ptr<std::uint32_t>> event;
  const auto launch = std::make_shared<std::uint32_t>(101);
  event.raise(launch);

  EXPECT_EQ(event.try_pop_if([](const auto &pending) {
    return *pending == 101;
  }),
            launch);
  EXPECT_FALSE(event.try_pop());
}

TEST(ThreadSafeEvent, EmptyAndStoppedEventsDoNotInvokePredicate) {
  safe::event_t<int> event;
  unsigned int inspections {};
  const auto predicate = [&](const auto &) {
    ++inspections;
    return true;
  };

  EXPECT_FALSE(event.try_pop_if(predicate));
  event.raise(101);
  event.stop();
  EXPECT_FALSE(event.try_pop_if(predicate));
  EXPECT_EQ(inspections, 0);
}

TEST(ThreadSafeEvent, OptionalValueCanBeConditionallyConsumed) {
  safe::event_t<int> event;
  event.raise(101);
  EXPECT_FALSE(event.try_pop_if([](const auto &pending) {
    return *pending == 202;
  }));
  const auto removed = event.try_pop_if([](const auto &pending) {
    return *pending == 101;
  });
  ASSERT_TRUE(removed);
  EXPECT_EQ(*removed, 101);
  EXPECT_FALSE(event.try_pop());
}

TEST(ThreadSafeEvent, ReplacementCannotInterleaveBetweenPredicateAndRemoval) {
  safe::event_t<int> event;
  event.raise(101);
  std::promise<void> inspecting;
  std::promise<void> release_inspection;
  auto released = release_inspection.get_future();
  auto removal = std::async(std::launch::async, [&]() {
    return event.try_pop_if([&](const auto &pending) {
      inspecting.set_value();
      released.wait();
      return *pending == 101;
    });
  });
  inspecting.get_future().wait();

  std::promise<void> producer_started;
  auto replacement = std::async(std::launch::async, [&]() {
    producer_started.set_value();
    event.raise(202);
  });
  producer_started.get_future().wait();
  EXPECT_EQ(replacement.wait_for(std::chrono::milliseconds {50}), std::future_status::timeout);
  release_inspection.set_value();

  const auto removed = removal.get();
  replacement.get();
  ASSERT_TRUE(removed);
  EXPECT_EQ(*removed, 101);
  const auto remaining = event.try_pop();
  ASSERT_TRUE(remaining);
  EXPECT_EQ(*remaining, 202);
}

TEST(ThreadSafeEvent, ConcurrentExpiryAndClearConsumeMatchingLaunchOnlyOnce) {
  safe::event_t<std::shared_ptr<std::uint32_t>> event;
  const auto launch = std::make_shared<std::uint32_t>(101);
  event.raise(launch);
  std::promise<void> start;
  const auto started = start.get_future().share();
  const auto consume = [&]() {
    started.wait();
    return event.try_pop_if([](const auto &pending) {
      return *pending == 101;
    });
  };
  auto expiry = std::async(std::launch::async, consume);
  auto clear = std::async(std::launch::async, consume);
  start.set_value();

  const auto expired = expiry.get();
  const auto cleared = clear.get();
  EXPECT_NE(static_cast<bool>(expired), static_cast<bool>(cleared));
  EXPECT_EQ(expired ? expired : cleared, launch);
  EXPECT_FALSE(event.try_pop());
}
