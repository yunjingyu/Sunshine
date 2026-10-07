/**
 * @file tests/unit/test_display_device_revert.cpp
 * @brief Exercise the production display restore callback and its real retry scheduler.
 */

// standard includes
#include <deque>
#include <future>
#include <thread>

// lib includes
#include <gtest/gtest.h>

// local includes
#include <src/display_device_revert.h>

namespace {
  using namespace std::chrono_literals;
  using display_device::detail::make_revert_callback;
  using display_device::detail::make_revert_scheduler_options;
  using display_device::SchedulerOptions;
  using display_device::SchedulerStopToken;
  using display_device::SettingsManagerInterface;
  using result_t = SettingsManagerInterface::RevertResult;

  /**
   * @brief Controlled settings interface for injecting failures into the production retry callback.
   */
  class RestoreSettings final: public SettingsManagerInterface {
  public:
    std::deque<RevertResult> results;  ///< Outcomes consumed in order; subsequent calls continue to fail.
    display_device::EnumeratedDeviceList devices;  ///< Enumeration stays unchanged while a restore is retried.
    unsigned int restore_calls {0};  ///< Number of actual restore attempts.
    unsigned int apply_calls {0};  ///< Number of new stream configurations applied.
    unsigned int reset_calls {0};  ///< Number of explicit persistence resets.

    /**
     * @brief Return the fixed device enumeration.
     */
    display_device::EnumeratedDeviceList enumAvailableDevices() const override {
      return devices;
    }

    /**
     * @brief Return an unused capture name.
     */
    std::string getDisplayName(const std::string &) const override {
      return {};
    }

    /**
     * @brief Record a new stream applying its configuration.
     */
    ApplyResult applySettings(const display_device::SingleDisplayConfiguration &) override {
      ++apply_calls;
      return ApplyResult::Ok;
    }

    /**
     * @brief Consume an injected restore outcome.
     */
    RevertResult revertSettings() override {
      ++restore_calls;
      if (results.empty()) {
        return RevertResult::SwitchingTopologyFailed;
      }
      const auto result {results.front()};
      results.pop_front();
      return result;
    }

    /**
     * @brief Record a user resetting persistence.
     */
    bool resetPersistence() override {
      ++reset_calls;
      return true;
    }
  };

  /**
   * @brief Report sink for tests that only inspect actual interface calls and scheduler state.
   */
  void ignore_report(result_t, std::optional<std::chrono::milliseconds>) {}
}  // namespace

TEST(DisplayDeviceRevertTest, ImmediateScheduleUsesCappedBackoff) {
  const auto options {make_revert_scheduler_options(0ms)};
  EXPECT_EQ(options.m_execution, SchedulerOptions::Execution::Immediate);
  EXPECT_EQ(options.m_sleep_durations, (std::vector<std::chrono::milliseconds> {5s, 10s, 20s, 40s, 60s}));
}

TEST(DisplayDeviceRevertTest, ConfiguredSixSecondDelayPrecedesBackoff) {
  const auto options {make_revert_scheduler_options(6s)};
  EXPECT_EQ(options.m_execution, SchedulerOptions::Execution::ScheduledOnly);
  EXPECT_EQ(options.m_sleep_durations, (std::vector<std::chrono::milliseconds> {6s, 5s, 10s, 20s, 40s, 60s}));
}

TEST(DisplayDeviceRevertTest, EmptyEnumerationDoesNotSkipFirstAttempt) {
  RestoreSettings settings;
  settings.results = {result_t::Ok};
  ASSERT_TRUE(settings.enumAvailableDevices().empty());
  auto callback {make_revert_callback(false, ignore_report)};
  SchedulerStopToken token {[]() {
  }};
  callback(settings, token);
  EXPECT_EQ(settings.restore_calls, 1u);
  EXPECT_TRUE(token.stopRequested());
}

TEST(DisplayDeviceRevertTest, NoSavedStateStopsWithoutReportingAnActualRestoration) {
  RestoreSettings settings;
  settings.results = {result_t::NoChangesToRevert};
  result_t reported {result_t::Ok};
  std::optional<std::chrono::milliseconds> delay;
  auto callback {make_revert_callback(false, [&](auto result, auto next_retry) {
    reported = result;
    delay = next_retry;
  })};
  SchedulerStopToken token {[]() {
  }};
  callback(settings, token);
  EXPECT_TRUE(token.stopRequested());
  EXPECT_EQ(reported, result_t::NoChangesToRevert);
  EXPECT_FALSE(delay);
  EXPECT_EQ(settings.restore_calls, 1u);
}

TEST(DisplayDeviceRevertTest, UnchangedDevicesDoNotBlockRecoveryAfterTopologyFailure) {
  RestoreSettings settings;
  settings.devices = {{.m_device_id = "physical-monitor", .m_friendly_name = "Physical monitor"}};
  settings.results = {result_t::SwitchingTopologyFailed, result_t::Ok};
  const auto original_devices {settings.enumAvailableDevices()};
  std::vector<std::optional<std::chrono::milliseconds>> reported_delays;
  auto callback {make_revert_callback(false, [&](const auto, const auto &delay) {
    reported_delays.push_back(delay);
  })};
  SchedulerStopToken first {[]() {
  }};
  callback(settings, first);
  EXPECT_FALSE(first.stopRequested());
  EXPECT_EQ(settings.enumAvailableDevices(), original_devices);
  SchedulerStopToken second {[]() {
  }};
  callback(settings, second);
  EXPECT_TRUE(second.stopRequested());
  EXPECT_EQ(settings.restore_calls, 2u);
  EXPECT_EQ(reported_delays, (std::vector<std::optional<std::chrono::milliseconds>> {5s, std::nullopt}));
}

TEST(DisplayDeviceRevertTest, EveryFailureKeepsRecoveryScheduledWithAccurateResult) {
  const std::vector<result_t> failures {
    result_t::ApiTemporarilyUnavailable,
    result_t::TopologyIsInvalid,
    result_t::SwitchingTopologyFailed,
    result_t::RevertingPrimaryDeviceFailed,
    result_t::RevertingDisplayModesFailed,
    result_t::RevertingHdrStatesFailed,
    result_t::PersistenceSaveFailed
  };
  RestoreSettings settings;
  settings.results = {failures.begin(), failures.end()};
  std::vector<result_t> reported_results;
  std::vector<std::optional<std::chrono::milliseconds>> reported_delays;
  auto callback {make_revert_callback(false, [&](const auto result, const auto &delay) {
    reported_results.push_back(result);
    reported_delays.push_back(delay);
  })};
  for (const auto failure : failures) {
    SCOPED_TRACE(static_cast<int>(failure));
    SchedulerStopToken token {[]() {
    }};
    callback(settings, token);
    EXPECT_FALSE(token.stopRequested());
  }
  EXPECT_EQ(reported_results, failures);
  EXPECT_EQ(reported_delays, (std::vector<std::optional<std::chrono::milliseconds>> {5s, 10s, 20s, 40s, 60s, 60s, 60s}));
}

TEST(DisplayDeviceRevertTest, ShutdownAttemptsOnceAndReportsFailureWithoutRetry) {
  RestoreSettings settings;
  settings.results = {result_t::SwitchingTopologyFailed};
  auto callback {make_revert_callback(true, [](const auto result, const auto &delay) {
    EXPECT_EQ(result, result_t::SwitchingTopologyFailed);
    EXPECT_FALSE(delay.has_value());
  })};
  SchedulerStopToken token {[]() {
  }};
  callback(settings, token);
  EXPECT_EQ(settings.restore_calls, 1u);
  EXPECT_TRUE(token.stopRequested());
}

TEST(DisplayDeviceRevertTest, RealSchedulerRetriesUntilSuccessThenStops) {
  std::promise<void> completed;
  auto completion {completed.get_future()};
  auto settings {std::make_unique<RestoreSettings>()};
  settings->results = {result_t::SwitchingTopologyFailed, result_t::ApiTemporarilyUnavailable, result_t::Ok};
  display_device::RetryScheduler<SettingsManagerInterface> scheduler {std::move(settings)};
  auto options {make_revert_scheduler_options(0ms)};
  // Shorten only the timer in this integration test; the production intervals are asserted above.
  options.m_sleep_durations = {1ms};
  scheduler.schedule(make_revert_callback(false, [&](const auto result, const auto &) {
                       if (result == result_t::Ok) {
                         completed.set_value();
                       }
                     }),
                     options);
  ASSERT_EQ(completion.wait_for(2s), std::future_status::ready);
  scheduler.execute([&](const auto &iface) {
    EXPECT_EQ(static_cast<const RestoreSettings &>(iface).restore_calls, 3u);
    EXPECT_FALSE(scheduler.isScheduled());
  });
}

TEST(DisplayDeviceRevertTest, RealSchedulerHonorsInitialDelay) {
  std::promise<std::chrono::steady_clock::time_point> attempted;
  auto attempt_time {attempted.get_future()};
  auto settings {std::make_unique<RestoreSettings>()};
  settings->results = {result_t::Ok};
  display_device::RetryScheduler<SettingsManagerInterface> scheduler {std::move(settings)};
  const auto before {std::chrono::steady_clock::now()};
  scheduler.schedule(make_revert_callback(false, [&](const auto, const auto &) {
                       attempted.set_value(std::chrono::steady_clock::now());
                     }),
                     make_revert_scheduler_options(25ms));
  ASSERT_EQ(attempt_time.wait_for(2s), std::future_status::ready);
  EXPECT_GE(attempt_time.get() - before, 25ms);
}

TEST(DisplayDeviceRevertTest, NewStreamApplyCancelsPendingRestore) {
  display_device::RetryScheduler<SettingsManagerInterface> scheduler {std::make_unique<RestoreSettings>()};
  scheduler.schedule(make_revert_callback(false, ignore_report), {.m_sleep_durations = {100ms}});
  scheduler.schedule([](auto &iface, auto &token) {
    EXPECT_EQ(iface.applySettings({}), SettingsManagerInterface::ApplyResult::Ok);
    token.requestStop();
  },
                     {.m_sleep_durations = {1ms}});
  const auto calls_after_apply {scheduler.execute([](const auto &iface) {
    return static_cast<const RestoreSettings &>(iface).restore_calls;
  })};
  std::this_thread::sleep_for(150ms);
  scheduler.execute([&](const auto &iface) {
    const auto &settings {static_cast<const RestoreSettings &>(iface)};
    EXPECT_EQ(settings.restore_calls, calls_after_apply);
    EXPECT_EQ(settings.apply_calls, 1u);
    EXPECT_FALSE(scheduler.isScheduled());
  });
}

TEST(DisplayDeviceRevertTest, ExplicitPersistenceResetCancelsPendingRestore) {
  display_device::RetryScheduler<SettingsManagerInterface> scheduler {std::make_unique<RestoreSettings>()};
  scheduler.schedule(make_revert_callback(false, ignore_report), {.m_sleep_durations = {100ms}});
  const auto calls_after_reset {scheduler.execute([](auto &iface, auto &token) {
    token.requestStop();
    EXPECT_TRUE(iface.resetPersistence());
    return static_cast<const RestoreSettings &>(iface).restore_calls;
  })};
  std::this_thread::sleep_for(150ms);
  scheduler.execute([&](const auto &iface) {
    const auto &settings {static_cast<const RestoreSettings &>(iface)};
    EXPECT_EQ(settings.restore_calls, calls_after_reset);
    EXPECT_EQ(settings.reset_calls, 1u);
    EXPECT_FALSE(scheduler.isScheduled());
  });
}

TEST(DisplayDeviceRevertTest, FailedShutdownAttemptCancelsEarlierRestoreSchedule) {
  display_device::RetryScheduler<SettingsManagerInterface> scheduler {std::make_unique<RestoreSettings>()};
  scheduler.schedule(make_revert_callback(false, ignore_report), {.m_sleep_durations = {100ms}});
  scheduler.schedule(make_revert_callback(true, ignore_report), make_revert_scheduler_options(0ms));
  const auto calls_after_shutdown {scheduler.execute([](const auto &iface) {
    return static_cast<const RestoreSettings &>(iface).restore_calls;
  })};
  std::this_thread::sleep_for(150ms);
  scheduler.execute([&](const auto &iface) {
    EXPECT_EQ(static_cast<const RestoreSettings &>(iface).restore_calls, calls_after_shutdown);
    EXPECT_FALSE(scheduler.isScheduled());
  });
}

TEST(DisplayDeviceRevertTest, ScreenSaverStartsOnlyAfterRestoreSucceeds) {
  RestoreSettings settings;
  settings.results = {result_t::SwitchingTopologyFailed, result_t::Ok};
  unsigned int starts {};
  auto callback = make_revert_callback(false, ignore_report, [&]() {
    ++starts;
    return true;
  });
  SchedulerStopToken first {[]() {
  }};
  callback(settings, first);
  EXPECT_EQ(starts, 0u);
  EXPECT_FALSE(first.stopRequested());
  SchedulerStopToken second {[]() {
  }};
  callback(settings, second);
  EXPECT_EQ(starts, 1u);
  EXPECT_TRUE(second.stopRequested());
}

TEST(DisplayDeviceRevertTest, UnverifiedScreenSaverStartIsRetriedWithoutFalseCompletion) {
  RestoreSettings settings;
  settings.results = {result_t::NoChangesToRevert, result_t::NoChangesToRevert};
  unsigned int starts {};
  auto callback = make_revert_callback(false, ignore_report, [&]() {
    return ++starts == 2;
  });
  SchedulerStopToken first {[]() {
  }};
  callback(settings, first);
  EXPECT_FALSE(first.stopRequested());
  SchedulerStopToken second {[]() {
  }};
  callback(settings, second);
  EXPECT_TRUE(second.stopRequested());
  EXPECT_EQ(starts, 2u);
}
