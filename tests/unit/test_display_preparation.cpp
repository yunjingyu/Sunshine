/**
 * @file tests/unit/test_display_preparation.cpp
 * @brief Verify the production desktop preparation policy without touching a live desktop.
 */

// standard includes
#include <algorithm>
#include <deque>
#include <string>
#include <vector>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/platform/windows/display_preparation.h"

namespace {
  using platf::detail::input_desktop_e;
  using platf::display_preparation_e;

  /**
   * @brief Deterministic replacements for native calls used by the actual preparation policy.
   */
  struct desktop_api_t {
    using desktop_t = int;
    std::vector<std::string> calls;
    std::vector<input_desktop_e> desktops {input_desktop_e::normal};
    std::deque<display_preparation_e> sessions;
    std::deque<std::optional<bool>> input_results;
    std::optional<bool> secure {false};
    display_preparation_e close_result {display_preparation_e::ready};
    bool wake_ok {true};
    bool open_ok {true};
    bool attach_ok {true};
    bool restore_ok {true};
    unsigned int waits {};
    unsigned int closes {};
    unsigned int suspensions {};
    unsigned int start_requests {};
    bool suspend_ok {true};
    bool start_ok {true};
    int current_desktop {10};
    std::deque<std::optional<bool>> running_results;
    std::optional<bool> running {false};

    /** @brief Record suspension of the normal screen saver for an active stream. */
    bool suspend_nonsecure_screen_saver() {
      ++suspensions;
      return suspend_ok;
    }

    /** @brief Inject the observed native running state, including query failures. */
    std::optional<bool> screen_saver_running() {
      if (running_results.empty()) {
        return running;
      }
      const auto value = running_results.front();
      running_results.pop_front();
      return value;
    }

    /** @brief Record the normal Windows start request without touching a live desktop. */
    bool request_screen_saver() {
      ++start_requests;
      return start_ok;
    }

    /**
     * @brief Record a one-shot display wake request.
     */
    bool wake_display() {
      calls.emplace_back("wake");
      return wake_ok;
    }

    /**
     * @brief Consume explicitly supplied lock transitions, otherwise remain unlocked.
     */
    display_preparation_e session_status() {
      if (sessions.empty()) {
        return display_preparation_e::ready;
      }
      const auto status = sessions.front();
      sessions.pop_front();
      return status;
    }

    /**
     * @brief Issue distinct input desktop handles for each observed transition.
     */
    int open_input_desktop() {
      calls.emplace_back("open");
      return open_ok ? 20 + waits : 0;
    }

    /**
     * @brief Read the simulated desktop at the current observation.
     */
    input_desktop_e desktop_kind(int) {
      return desktops[std::min<size_t>(waits, desktops.size() - 1)];
    }

    /**
     * @brief Supply the simulated screen saver password policy.
     */
    std::optional<bool> screen_saver_secure() {
      return secure;
    }

    /**
     * @brief Model a desktop transition racing with attachment or inspection.
     */
    std::optional<bool> is_input_desktop(int) {
      if (input_results.empty()) {
        return true;
      }
      const auto input = input_results.front();
      input_results.pop_front();
      return input;
    }

    /**
     * @brief Record a screen saver close request without posting a Windows message.
     */
    display_preparation_e close_screen_saver(int desktop) {
      ++closes;
      if (desktop != current_desktop) {
        return display_preparation_e::failed;
      }
      return close_result;
    }

    /**
     * @brief Return the borrowed original worker desktop.
     */
    int thread_desktop() {
      return 10;
    }

    /**
     * @brief Record worker attachment and restoration separately.
     */
    bool set_thread_desktop(int desktop) {
      calls.emplace_back("attach:" + std::to_string(desktop));
      const bool success = desktop == 10 ? restore_ok : attach_ok;
      if (success) {
        current_desktop = desktop;
      }
      return success;
    }

    /**
     * @brief Record release of owned input handles.
     */
    void close_desktop(int desktop) {
      calls.emplace_back("close:" + std::to_string(desktop));
    }

    /**
     * @brief Advance the simulated desktop without sleeping.
     */
    void wait_for_desktop() {
      ++waits;
    }

    /**
     * @brief Record policy failures that require a diagnostic.
     */
    void failure(const char *) {
      calls.emplace_back("failure");
    }
  };

  TEST(DisplayPreparationTest, NormalDesktopStaysAttachedUntilApplyScopeEnds) {
    desktop_api_t api;
    auto prepared = platf::detail::prepare_display_configuration(api);
    ASSERT_EQ(prepared.status, display_preparation_e::ready);
    ASSERT_NE(prepared.guard, nullptr);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake", "open", "attach:20"}));
    prepared.guard.reset();
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake", "open", "attach:20", "attach:10", "close:20"}));
    EXPECT_EQ(api.closes, 0);
  }

  TEST(DisplayPreparationTest, LockedSessionIsDeferredWithoutOpeningOrDismissing) {
    desktop_api_t api;
    api.sessions = {display_preparation_e::secure_desktop};
    const auto prepared = platf::detail::prepare_display_configuration(api);
    EXPECT_EQ(prepared.status, display_preparation_e::secure_desktop);
    EXPECT_EQ(prepared.guard, nullptr);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake"}));
    EXPECT_EQ(api.closes, 0);
  }

  TEST(DisplayPreparationTest, PasswordProtectedSaverIsNeverDismissedOrAttached) {
    desktop_api_t api;
    api.desktops = {input_desktop_e::screen_saver};
    api.secure = true;
    const auto prepared = platf::detail::prepare_display_configuration(api);
    EXPECT_EQ(prepared.status, display_preparation_e::secure_desktop);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake", "open", "close:20"}));
    EXPECT_EQ(api.closes, 0);
  }

  TEST(DisplayPreparationTest, WinlogonDesktopIsNeverAttached) {
    desktop_api_t api;
    api.desktops = {input_desktop_e::secure};
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::secure_desktop);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake", "open", "close:20"}));
    EXPECT_EQ(api.closes, 0);
  }

  TEST(DisplayPreparationTest, NonsecureSaverIsClosedOnceAndNewInputDesktopIsVerified) {
    desktop_api_t api;
    api.desktops = {input_desktop_e::screen_saver, input_desktop_e::screen_saver, input_desktop_e::normal};
    auto prepared = platf::detail::prepare_display_configuration(api);
    ASSERT_EQ(prepared.status, display_preparation_e::ready);
    EXPECT_EQ(api.closes, 1);
    EXPECT_EQ(api.waits, 2);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake", "open", "attach:20", "attach:10", "close:20", "open", "close:21", "open", "attach:22"}));
    prepared.guard.reset();
    EXPECT_EQ(api.calls.back(), "close:22");
  }

  TEST(DisplayPreparationTest, UnresponsiveSaverFailsAfterBoundedWaitWithoutRepeatedClose) {
    desktop_api_t api;
    api.desktops = {input_desktop_e::screen_saver};
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::failed);
    EXPECT_EQ(api.closes, 1);
    EXPECT_EQ(api.waits, 30);
    EXPECT_EQ(api.calls.back(), "failure");
    EXPECT_EQ(std::count(api.calls.begin(), api.calls.end(), "attach:10"), 1);
  }

  TEST(DisplayPreparationTest, CloseFailureDoesNotContinueToDisplayConfiguration) {
    desktop_api_t api;
    api.desktops = {input_desktop_e::screen_saver};
    api.close_result = display_preparation_e::failed;
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::failed);
    EXPECT_EQ(api.waits, 0);
    EXPECT_EQ(api.closes, 1);
    EXPECT_EQ(api.calls.back(), "close:20");
  }

  TEST(DisplayPreparationTest, LockRaceImmediatelyBeforeCloseIsDeferred) {
    desktop_api_t api;
    api.desktops = {input_desktop_e::screen_saver};
    api.close_result = display_preparation_e::secure_desktop;
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::secure_desktop);
    EXPECT_EQ(api.waits, 0);
    EXPECT_EQ(api.calls.back(), "close:20");
  }

  TEST(DisplayPreparationTest, UnknownSessionStateIsFailureNotPermissionToDismiss) {
    desktop_api_t api;
    api.sessions = {display_preparation_e::failed};
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::failed);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake"}));
    EXPECT_EQ(api.closes, 0);
  }

  TEST(DisplayPreparationTest, InaccessibleDesktopIsNotAssumedToBeLocked) {
    desktop_api_t api;
    api.open_ok = false;
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::failed);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake", "open"}));
  }

  TEST(DisplayPreparationTest, LockBetweenSessionQueryAndOpenIsIdentified) {
    desktop_api_t api;
    api.open_ok = false;
    api.sessions = {display_preparation_e::ready, display_preparation_e::secure_desktop};
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::secure_desktop);
  }

  TEST(DisplayPreparationTest, UnknownPasswordPolicyCannotDismissSaver) {
    desktop_api_t api;
    api.desktops = {input_desktop_e::screen_saver};
    api.secure = std::nullopt;
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::failed);
    EXPECT_EQ(api.closes, 0);
    EXPECT_EQ(api.waits, 0);
  }

  TEST(DisplayPreparationTest, AttachmentFailureReleasesOnlyOwnedInputHandle) {
    desktop_api_t api;
    api.attach_ok = false;
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::failed);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake", "open", "attach:20", "close:20"}));
  }

  TEST(DisplayPreparationTest, FailedInputVerificationRestoresOriginalWorkerDesktop) {
    desktop_api_t api;
    api.input_results = {std::nullopt};
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::failed);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake", "open", "attach:20", "attach:10", "close:20"}));
  }

  TEST(DisplayPreparationTest, DesktopSwitchDuringAttachmentRestoresThenRechecksSecurity) {
    desktop_api_t api;
    api.desktops = {input_desktop_e::normal, input_desktop_e::secure};
    api.input_results = {false};
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::secure_desktop);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake", "open", "attach:20", "attach:10", "close:20", "open", "close:21"}));
    EXPECT_EQ(api.closes, 0);
  }

  TEST(DisplayPreparationTest, FailedRestorationDoesNotCloseTheStillAttachedHandle) {
    desktop_api_t api;
    auto prepared = platf::detail::prepare_display_configuration(api);
    ASSERT_EQ(prepared.status, display_preparation_e::ready);
    api.restore_ok = false;
    prepared.guard.reset();
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake", "open", "attach:20", "attach:10", "failure"}));
  }

  TEST(DisplayPreparationTest, UnknownDesktopCannotBeAttachedOrDismissed) {
    desktop_api_t api;
    api.desktops = {input_desktop_e::unknown};
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::failed);
    EXPECT_EQ(api.closes, 0);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake", "open", "failure", "close:20"}));
  }

  TEST(DisplayPreparationTest, FailedWakeDoesNotProceedWithDesktopConfiguration) {
    desktop_api_t api;
    api.wake_ok = false;
    EXPECT_EQ(platf::detail::prepare_display_configuration(api).status, display_preparation_e::failed);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"wake"}));
  }

  TEST(DisplayPreparationTest, NormalDesktopSuspendsSaverBeforeConfiguration) {
    desktop_api_t api;
    auto prepared = platf::detail::prepare_display_configuration(api);
    EXPECT_EQ(prepared.status, display_preparation_e::ready);
    EXPECT_EQ(api.suspensions, 1u);
  }

  TEST(DisplayPreparationTest, SuspensionFailureCannotStartCaptureWithAnActiveSaver) {
    desktop_api_t api;
    api.suspend_ok = false;
    auto prepared = platf::detail::prepare_display_configuration(api);
    EXPECT_EQ(prepared.status, display_preparation_e::failed);
    EXPECT_EQ(prepared.guard, nullptr);
    EXPECT_EQ(api.calls.back(), "close:20");
  }

  TEST(ScreenSaverStartTest, NormalDesktopRequestsAndObservesActualStartup) {
    desktop_api_t api;
    api.running_results = {false, false, true};
    EXPECT_TRUE(platf::detail::start_screen_saver(api));
    EXPECT_EQ(api.start_requests, 1u);
    EXPECT_EQ(api.waits, 1u);
    EXPECT_EQ(api.suspensions, 0u);
    EXPECT_EQ(api.calls.back(), "close:20");
  }

  TEST(ScreenSaverStartTest, RequestSuccessWithoutRunningReadbackIsFailure) {
    desktop_api_t api;
    EXPECT_FALSE(platf::detail::start_screen_saver(api));
    EXPECT_EQ(api.start_requests, 1u);
    EXPECT_EQ(api.waits, 30u);
  }

  TEST(ScreenSaverStartTest, AlreadyRunningSaverDoesNotRestart) {
    desktop_api_t api;
    api.running = true;
    EXPECT_TRUE(platf::detail::start_screen_saver(api));
    EXPECT_EQ(api.start_requests, 0u);
  }

  TEST(ScreenSaverStartTest, LockedSessionIsProtectedWithoutOpeningOrUnlocking) {
    desktop_api_t api;
    api.sessions = {display_preparation_e::secure_desktop};
    EXPECT_TRUE(platf::detail::start_screen_saver(api));
    EXPECT_TRUE(api.calls.empty());
    EXPECT_EQ(api.start_requests, 0u);
  }

  TEST(ScreenSaverStartTest, ExistingSaverDesktopIsNotAttachedOrClosed) {
    desktop_api_t api;
    api.desktops = {input_desktop_e::screen_saver};
    EXPECT_TRUE(platf::detail::start_screen_saver(api));
    EXPECT_EQ(api.closes, 0u);
    EXPECT_EQ(api.calls, (std::vector<std::string> {"open", "close:20"}));
  }

  TEST(ScreenSaverStartTest, UnknownDesktopCannotReceiveStartCommand) {
    desktop_api_t api;
    api.desktops = {input_desktop_e::unknown};
    EXPECT_FALSE(platf::detail::start_screen_saver(api));
    EXPECT_EQ(api.start_requests, 0u);
  }

  TEST(ScreenSaverStartTest, DesktopSwitchBeforeRequestCannotStartOnStaleDesktop) {
    desktop_api_t api;
    api.input_results = {false};
    EXPECT_FALSE(platf::detail::start_screen_saver(api));
    EXPECT_EQ(api.start_requests, 0u);
  }

  TEST(ScreenSaverStartTest, NativeStartAndQueryFailuresRemainFailures) {
    desktop_api_t api;
    api.start_ok = false;
    EXPECT_FALSE(platf::detail::start_screen_saver(api));
    api.start_ok = true;
    api.running = std::nullopt;
    EXPECT_FALSE(platf::detail::start_screen_saver(api));
  }
}  // namespace
