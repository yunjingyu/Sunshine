/**
 * @file tests/unit/test_display_device_apply.cpp
 * @brief Exercise display preparation, scope ownership, and application failure propagation.
 */

// standard includes
#include <memory>
#include <stdexcept>

// lib includes
#include <gtest/gtest.h>

// local includes
#include <src/display_device_apply.h>

namespace {
  using display_device::configuration_result_e;
  using display_device::SettingsManagerInterface;
  using display_device::SingleDisplayConfiguration;
  using apply_result_t = SettingsManagerInterface::ApplyResult;

  /**
   * @brief Scoped desktop attachment stand-in used to assert the production attempt's lifetime.
   */
  struct DesktopGuard {
    bool &attached;  ///< Attachment state checked from the settings interface.

    explicit DesktopGuard(bool &value):
        attached {value} {
      attached = true;
    }

    ~DesktopGuard() {
      attached = false;
    }
  };

  /**
   * @brief Preparation outcome matching the platform contract without touching a real desktop.
   */
  struct Preparation {
    configuration_result_e status;  ///< Preparation outcome.
    std::unique_ptr<DesktopGuard> guard;  ///< Worker-local attachment lifetime.
  };

  /**
   * @brief Settings implementation recording which production operations were reached.
   */
  class Settings final: public SettingsManagerInterface {
  public:
    bool attached {false};  ///< True only while the preparation guard exists.
    bool throw_apply {false};  ///< Inject an exception from the actual apply boundary.
    int apply_calls {0};  ///< Number of calls that reached configuration.
    ApplyResult result {ApplyResult::Ok};  ///< Actual result reported to the caller.
    display_device::EnumeratedDeviceList devices;  ///< Read-only verification input.
    SingleDisplayConfiguration received;  ///< Configuration delivered to the settings implementation.

    /** @brief Enumerate controlled device state. */
    display_device::EnumeratedDeviceList enumAvailableDevices() const override {
      return devices;
    }

    /** @brief Return an unused capture selector. */
    std::string getDisplayName(const std::string &) const override {
      return {};
    }

    /** @brief Check attachment lifetime and propagate the selected native outcome. */
    ApplyResult applySettings(const SingleDisplayConfiguration &config) override {
      EXPECT_TRUE(attached);
      ++apply_calls;
      received = config;
      if (throw_apply) {
        throw std::runtime_error {"apply failure"};
      }
      return result;
    }

    /** @brief Provide the unused restoration interface. */
    RevertResult revertSettings() override {
      return RevertResult::NoChangesToRevert;
    }

    /** @brief Provide the unused reset interface. */
    bool resetPersistence() override {
      return true;
    }
  };

  /** @brief Request the same exclusive-display/manual-resolution path as the affected host. */
  SingleDisplayConfiguration streaming_config() {
    return {.m_device_id = "vdd", .m_device_prep = SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay, .m_resolution = display_device::Resolution {2184, 1968}};
  }

  /** @brief Ignore diagnostics in tests that inspect call boundaries directly. */
  void ignore_report(configuration_result_e, std::optional<apply_result_t>) {}
}  // namespace

TEST(DisplayDeviceApplyTest, ConfiguresOnlyAfterPreparationAndRetainsGuardUntilApplyReturns) {
  Settings settings;
  auto attempt {display_device::detail::make_apply_attempt(streaming_config(), [&]() {
    return Preparation {configuration_result_e::ready, std::make_unique<DesktopGuard>(settings.attached)};
  },
                                                           ignore_report)};
  EXPECT_EQ(attempt(settings), configuration_result_e::ready);
  EXPECT_EQ(settings.apply_calls, 1);
  EXPECT_EQ(settings.received, streaming_config());
  EXPECT_FALSE(settings.attached);
}

TEST(DisplayDeviceApplyTest, VerifiedSecureDesktopDefersWithoutChangingDisplays) {
  Settings settings;
  auto attempt {display_device::detail::make_apply_attempt(streaming_config(), []() {
    return Preparation {configuration_result_e::secure_desktop, nullptr};
  },
                                                           ignore_report)};
  EXPECT_EQ(attempt(settings), configuration_result_e::secure_desktop);
  EXPECT_EQ(settings.apply_calls, 0);
}

TEST(DisplayDeviceApplyTest, UnknownPreparationFailureDoesNotMasqueradeAsSecureLogin) {
  Settings settings;
  auto attempt {display_device::detail::make_apply_attempt(streaming_config(), []() {
    return Preparation {configuration_result_e::failed, nullptr};
  },
                                                           ignore_report)};
  EXPECT_EQ(attempt(settings), configuration_result_e::failed);
  EXPECT_EQ(settings.apply_calls, 0);
}

TEST(DisplayDeviceApplyTest, EveryLibraryFailureRejectsStaleCaptureAndReportsActualResult) {
  const apply_result_t failures[] {apply_result_t::ApiTemporarilyUnavailable, apply_result_t::DevicePrepFailed, apply_result_t::PrimaryDevicePrepFailed, apply_result_t::DisplayModePrepFailed, apply_result_t::HdrStatePrepFailed, apply_result_t::PersistenceSaveFailed};
  for (const auto failure : failures) {
    Settings settings;
    settings.result = failure;
    std::optional<apply_result_t> reported;
    auto attempt {display_device::detail::make_apply_attempt(streaming_config(), [&]() {
      return Preparation {configuration_result_e::ready, std::make_unique<DesktopGuard>(settings.attached)};
    },
                                                             [&](auto status, auto actual) {
                                                               EXPECT_EQ(status, configuration_result_e::failed);
                                                               reported = actual;
                                                             })};
    EXPECT_EQ(attempt(settings), configuration_result_e::failed);
    EXPECT_EQ(reported, failure);
    EXPECT_FALSE(settings.attached);
  }
}

TEST(DisplayDeviceApplyTest, NativeExceptionReleasesDesktopAttachment) {
  Settings settings;
  settings.throw_apply = true;
  auto attempt {display_device::detail::make_apply_attempt(streaming_config(), [&]() {
    return Preparation {configuration_result_e::ready, std::make_unique<DesktopGuard>(settings.attached)};
  },
                                                           ignore_report)};
  EXPECT_THROW(attempt(settings), std::runtime_error);
  EXPECT_FALSE(settings.attached);
}

TEST(DisplayDeviceApplyTest, VerifyOnlyReadsActiveDeviceWithoutInvokingConfigurationApi) {
  Settings settings;
  settings.devices = {{.m_device_id = "vdd", .m_info = display_device::EnumeratedDevice::Info {}}};
  SingleDisplayConfiguration config {.m_device_id = "vdd", .m_device_prep = SingleDisplayConfiguration::DevicePreparation::VerifyOnly};
  auto attempt {display_device::detail::make_apply_attempt(config, []() {
    return Preparation {configuration_result_e::ready, nullptr};
  },
                                                           ignore_report)};
  EXPECT_EQ(attempt(settings), configuration_result_e::ready);
  EXPECT_EQ(settings.apply_calls, 0);
  settings.devices.front().m_info.reset();
  EXPECT_EQ(attempt(settings), configuration_result_e::failed);
  EXPECT_EQ(settings.apply_calls, 0);
}

TEST(DisplayDeviceApplyTest, VerifyOnlyWithoutIdRequiresAnActivePrimaryDevice) {
  Settings settings;
  settings.devices = {{.m_device_id = "vdd", .m_info = display_device::EnumeratedDevice::Info {}}};
  auto attempt {display_device::detail::make_apply_attempt(SingleDisplayConfiguration {}, []() {
    return Preparation {configuration_result_e::ready, nullptr};
  },
                                                           ignore_report)};
  EXPECT_EQ(attempt(settings), configuration_result_e::failed);
  settings.devices.front().m_info->m_primary = true;
  EXPECT_EQ(attempt(settings), configuration_result_e::ready);
  EXPECT_EQ(settings.apply_calls, 0);
}

TEST(DisplayDeviceApplyTest, VerifyOnlyWithRequestedResolutionStillAppliesIt) {
  Settings settings;
  auto config {streaming_config()};
  config.m_device_prep = SingleDisplayConfiguration::DevicePreparation::VerifyOnly;
  auto attempt {display_device::detail::make_apply_attempt(config, [&]() {
    return Preparation {configuration_result_e::ready, std::make_unique<DesktopGuard>(settings.attached)};
  },
                                                           ignore_report)};
  EXPECT_EQ(attempt(settings), configuration_result_e::ready);
  EXPECT_EQ(settings.apply_calls, 1);
}
