/** @file tests/unit/test_display_device_idle.cpp
 * @brief Verify the production last-disconnect generation and retry policy.
 */
#include "src/display_device_idle.h"

#include <gtest/gtest.h>

TEST(DisplayDeviceIdleTest, OnlyCurrentGenerationCanRequestTheScreenSaver) {
  display_device::detail::idle_screen_saver_t idle;
  idle.begin_connection();
  const auto old = idle.generation();
  idle.begin_connection();
  EXPECT_FALSE(idle.request(old));
  unsigned int starts {};
  EXPECT_TRUE(idle.complete([&]() {
    ++starts;
    return true;
  }));
  EXPECT_EQ(starts, 0u);
  EXPECT_TRUE(idle.request(idle.generation()));
  EXPECT_TRUE(idle.complete([&]() {
    ++starts;
    return true;
  }));
  EXPECT_EQ(starts, 1u);
}

TEST(DisplayDeviceIdleTest, ReconnectCancelsPendingIdleAction) {
  display_device::detail::idle_screen_saver_t idle;
  idle.begin_connection();
  ASSERT_TRUE(idle.request(idle.generation()));
  idle.begin_connection();
  EXPECT_TRUE(idle.complete([]() {
    ADD_FAILURE() << "Old disconnect started the saver after reconnect";
    return false;
  }));
}

TEST(DisplayDeviceIdleTest, NativeFailureRetainsRequestUntilVerifiedSuccess) {
  display_device::detail::idle_screen_saver_t idle;
  idle.begin_connection();
  ASSERT_TRUE(idle.request(idle.generation()));
  EXPECT_FALSE(idle.complete([]() {
    return false;
  }));
  unsigned int starts {};
  EXPECT_TRUE(idle.complete([&]() {
    ++starts;
    return true;
  }));
  EXPECT_TRUE(idle.complete([&]() {
    ++starts;
    return true;
  }));
  EXPECT_EQ(starts, 1u);
}

TEST(DisplayDeviceIdleTest, ExplicitResetCancelsIdleAction) {
  display_device::detail::idle_screen_saver_t idle;
  ASSERT_TRUE(idle.request(idle.generation()));
  idle.cancel();
  EXPECT_TRUE(idle.complete([]() {
    ADD_FAILURE();
    return false;
  }));
}
