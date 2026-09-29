/**
 * @file src/display_device_revert.h
 * @brief Retry policy for restoring display settings after a stream.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <optional>
#include <utility>

// lib includes
#include <display_device/retry_scheduler.h>
#include <display_device/settings_manager_interface.h>

namespace display_device::detail {
  /**
   * @brief Retry transient restore failures without requiring a display hotplug event.
   * @note The final interval repeats indefinitely, limiting disruptive topology changes during prolonged failures.
   */
  inline constexpr std::array<std::chrono::milliseconds, 5> revert_retry_intervals {
    std::chrono::seconds {5},
    std::chrono::seconds {10},
    std::chrono::seconds {20},
    std::chrono::seconds {40},
    std::chrono::seconds {60}
  };

  /**
   * @brief Build the restore schedule, preserving the configured delay before the first attempt.
   * @param initial_delay Delay before the first attempt, or zero to attempt immediately.
   * @return Scheduler options with capped backoff after each failed attempt.
   */
  inline SchedulerOptions make_revert_scheduler_options(std::chrono::milliseconds initial_delay) {
    SchedulerOptions options {
      .m_sleep_durations = {revert_retry_intervals.begin(), revert_retry_intervals.end()}
    };
    if (initial_delay > std::chrono::milliseconds::zero()) {
      options.m_sleep_durations.insert(options.m_sleep_durations.begin(), initial_delay);
      options.m_execution = SchedulerOptions::Execution::ScheduledOnly;
    }
    return options;
  }

  /**
   * @brief Create the callback used by the restore scheduler.
   * @tparam ReportT Callable accepting the actual restore result and optional delay before the next attempt.
   * @param try_once Stop after one attempt, including failures, for shutdown and reinitialization.
   * @param report Reports each attempted restore; an empty delay means no further attempts are scheduled.
   * @return Stateful callback that stops on success and retries failures independently of device enumeration.
   */
  template<class ReportT>
  auto make_revert_callback(bool try_once, ReportT report) {
    return [try_once, report = std::move(report), interval_index = std::size_t {0}](SettingsManagerInterface &settings_iface, SchedulerStopToken &stop_token) mutable {
      const auto result {settings_iface.revertSettings()};
      std::optional<std::chrono::milliseconds> next_retry;
      if (try_once || result == SettingsManagerInterface::RevertResult::Ok || result == SettingsManagerInterface::RevertResult::NoChangesToRevert) {
        stop_token.requestStop();
      } else {
        next_retry = revert_retry_intervals[interval_index];
        if (interval_index + 1 < revert_retry_intervals.size()) {
          ++interval_index;
        }
      }
      report(result, next_retry);
    };
  }
}  // namespace display_device::detail
