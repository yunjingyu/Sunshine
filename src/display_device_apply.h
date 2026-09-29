/**
 * @file src/display_device_apply.h
 * @brief Display preparation and application policy shared by initial and deferred attempts.
 */
#pragma once

// standard includes
#include <algorithm>
#include <optional>
#include <utility>

// lib includes
#include <display_device/settings_manager_interface.h>

// local includes
#include "display_device.h"

namespace display_device::detail {
  /**
   * @brief Create an application attempt that keeps the desktop guard alive through configuration.
   * @tparam PrepareT Callable returning a scoped object with a ready/secure_desktop/failed status.
   * @tparam ReportT Callable receiving the final status and optional actual library result.
   * @param config Requested display settings, copied for deferred execution.
   * @param prepare Prepares only the calling worker's desktop and owns its restoration guard.
   * @param report Records success, verified secure deferral, or the actual failure.
   * @return Callable applying the configuration to the provided settings interface.
   */
  template<class PrepareT, class ReportT>
  auto make_apply_attempt(SingleDisplayConfiguration config, PrepareT prepare, ReportT report) {
    return [config = std::move(config), prepare = std::move(prepare), report = std::move(report)](SettingsManagerInterface &settings_iface) {
      auto context {prepare()};
      using preparation_status_t = decltype(context.status);
      if (context.status != preparation_status_t::ready) {
        const auto result {context.status == preparation_status_t::secure_desktop ? configuration_result_e::secure_desktop : configuration_result_e::failed};
        report(result, std::optional<SettingsManagerInterface::ApplyResult> {});
        return result;
      }
      if (config.m_device_prep == SingleDisplayConfiguration::DevicePreparation::VerifyOnly && !config.m_resolution && !config.m_refresh_rate && !config.m_hdr_state) {
        const auto devices {settings_iface.enumAvailableDevices()};
        const bool active {std::ranges::any_of(devices, [&config](const auto &device) {
          return device.m_info && (config.m_device_id.empty() ? device.m_info->m_primary : device.m_device_id == config.m_device_id);
        })};
        const auto result {active ? configuration_result_e::ready : configuration_result_e::failed};
        report(result, active ? std::optional<SettingsManagerInterface::ApplyResult> {} : std::optional {SettingsManagerInterface::ApplyResult::DevicePrepFailed});
        return result;
      }
      const auto applied {settings_iface.applySettings(config)};
      const auto result {applied == SettingsManagerInterface::ApplyResult::Ok ? configuration_result_e::ready : configuration_result_e::failed};
      report(result, std::optional {applied});
      return result;
    };
  }
}  // namespace display_device::detail
