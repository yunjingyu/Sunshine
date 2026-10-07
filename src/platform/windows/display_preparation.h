/**
 * @file src/platform/windows/display_preparation.h
 * @brief Prepare a display configuration worker without switching the user's desktop.
 */
#pragma once

// standard includes
#include <memory>
#include <optional>
#include <utility>

namespace platf {
  /**
   * @brief Distinguish an accessible desktop from a security boundary or a preparation error.
   */
  enum class display_preparation_e {
    ready,  ///< The calling worker is attached to the verified normal input desktop.
    secure_desktop,  ///< Windows authentication or a secure desktop prevents configuration.
    failed  ///< Preparation failed without positively identifying a security boundary.
  };

  /**
   * @brief Keep a worker attached to its prepared desktop until display configuration finishes.
   */
  class display_preparation_guard_t {
  public:
    /**
     * @brief Restore the original thread desktop on the same thread that prepared it.
     */
    virtual ~display_preparation_guard_t() = default;
  };

  /**
   * @brief Preparation outcome and the worker's scoped desktop attachment.
   */
  struct display_preparation_t {
    display_preparation_e status {display_preparation_e::failed};  ///< Result of desktop preparation.
    std::unique_ptr<display_preparation_guard_t> guard;  ///< Attachment retained until configuration finishes.
  };

  /**
   * @brief Wake the display, dismiss a nonsecure screen saver, and prepare only the calling worker.
   * @return A ready guard, a confirmed secure desktop, or a failure with a diagnostic log.
   * @note Call only for an intentional stream start on a worker without windows or hooks.
   *       Destroy the returned guard on that same worker after display configuration.
   */
  [[nodiscard]] display_preparation_t prepare_display_configuration();

  /**
   * @brief Start the configured Windows screen saver and verify that it becomes active.
   * @return True if the saver is active or Windows is already securely locked.
   * @note Call after the last stream stops and display restoration succeeds; never unlocks Windows.
   */
  [[nodiscard]] bool start_screen_saver();

  namespace detail {
    /**
     * @brief Recognized input desktop types; other desktops must not be modified.
     */
    enum class input_desktop_e {
      normal,  ///< The normal Windows Default desktop.
      screen_saver,  ///< The dedicated Screen-saver desktop, subject to password policy checks.
      secure,  ///< The Windows Winlogon desktop.
      unknown  ///< A desktop not identified as safe for this operation.
    };

    /**
     * @brief Own an input desktop handle and optionally restore a previous thread attachment.
     * @tparam Api Native desktop operations, also replaceable by deterministic test operations.
     */
    template<class Api>
    class display_desktop_guard_t final: public display_preparation_guard_t {
    public:
      /**
       * @brief Own a desktop handle without yet attaching the calling worker.
       */
      display_desktop_guard_t(Api &api, typename Api::desktop_t desktop):
          m_api {api},
          m_desktop {desktop} {
      }

      /**
       * @brief Restore the worker before closing its owned desktop handle.
       */
      ~display_desktop_guard_t() override {
        // CloseDesktop cannot close a handle that a thread is still using.
        if (m_original && !m_api.set_thread_desktop(m_original)) {
          m_api.failure("could not restore the display worker's original desktop; retaining its active handle");
          return;
        }
        m_api.close_desktop(m_desktop);
      }

      /**
       * @brief Attach only this worker and remember its original, borrowed desktop handle.
       * @return Whether the worker is attached successfully.
       */
      bool attach() {
        const auto original = m_api.thread_desktop();
        if (!original || !m_api.set_thread_desktop(m_desktop)) {
          return false;
        }
        m_original = original;
        return true;
      }

    private:
      Api &m_api;  ///< Native operations that outlive this guard.
      typename Api::desktop_t m_desktop;  ///< Owned input desktop handle.
      typename Api::desktop_t m_original {};  ///< Borrowed original desktop, set only after attachment.
    };

    /**
     * @brief Start the screen saver only on a verified normal input desktop.
     * @param api Native operations that outlive the scoped worker attachment.
     * @return Whether Windows is protected by an active saver or an existing secure desktop.
     */
    template<class Api>
    bool start_screen_saver(Api &api) {
      const auto session = api.session_status();
      if (session == display_preparation_e::secure_desktop) {
        return true;
      }
      if (session != display_preparation_e::ready) {
        return false;
      }
      const auto desktop = api.open_input_desktop();
      if (!desktop) {
        return false;
      }
      auto guard = std::make_unique<display_desktop_guard_t<Api>>(api, desktop);
      const auto kind = api.desktop_kind(desktop);
      if (kind == input_desktop_e::secure || kind == input_desktop_e::screen_saver) {
        return true;
      }
      if (kind != input_desktop_e::normal || !guard->attach()) {
        api.failure("cannot start screen saver on an unverified input desktop");
        return false;
      }
      const auto input = api.is_input_desktop(desktop);
      if (!input || !*input || api.session_status() != display_preparation_e::ready) {
        return false;
      }
      auto running = api.screen_saver_running();
      if (!running) {
        return false;
      }
      if (*running) {
        return true;
      }
      if (!api.request_screen_saver()) {
        return false;
      }
      for (unsigned int attempt = 0; attempt <= 30; ++attempt) {
        running = api.screen_saver_running();
        if (!running) {
          return false;
        }
        if (*running || api.session_status() == display_preparation_e::secure_desktop) {
          return true;
        }
        if (attempt < 30) {
          api.wait_for_desktop();
        }
      }
      api.failure("screen saver did not become active after SC_SCREENSAVE; retaining the idle request for retry");
      return false;
    }

    /**
     * @brief Run the production preparation policy with injectable native operations.
     * @param api Operations that must outlive the returned guard.
     * @return A scoped, verified attachment or a reason not to configure displays.
     */
    template<class Api>
    display_preparation_t prepare_display_configuration(Api &api) {
      if (!api.wake_display()) {
        return {};
      }

      bool close_requested {false};
      // PostMessage never waits for a screen saver. Observe its desktop transition
      // for at most 30 short waits before suspending its nonsecure idle timer.
      for (unsigned int attempt = 0; attempt <= 30; ++attempt) {
        const auto session = api.session_status();
        if (session != display_preparation_e::ready) {
          return {session, nullptr};
        }

        const auto desktop = api.open_input_desktop();
        if (!desktop) {
          // The session may have locked between its query and OpenInputDesktop.
          const auto latest_session = api.session_status();
          return {latest_session == display_preparation_e::secure_desktop ? latest_session : display_preparation_e::failed, nullptr};
        }
        auto guard = std::make_unique<display_desktop_guard_t<Api>>(api, desktop);
        const auto kind = api.desktop_kind(desktop);
        if (kind == input_desktop_e::secure) {
          return {display_preparation_e::secure_desktop, nullptr};
        }
        if (kind == input_desktop_e::normal) {
          if (!guard->attach()) {
            return {};
          }
          const auto latest_session = api.session_status();
          if (latest_session != display_preparation_e::ready) {
            return {latest_session, nullptr};
          }
          const auto input = api.is_input_desktop(desktop);
          if (!input) {
            return {};
          }
          if (*input) {
            if (!api.suspend_nonsecure_screen_saver()) {
              return {};
            }
            return {display_preparation_e::ready, std::move(guard)};
          }
          // The input desktop changed while the worker was attaching. Re-query
          // it instead of configuring a stale or newly secured desktop.
        } else if (kind == input_desktop_e::screen_saver) {
          const auto secure = api.screen_saver_secure();
          if (!secure) {
            return {};
          }
          if (*secure) {
            return {display_preparation_e::secure_desktop, nullptr};
          }
          const auto latest_session = api.session_status();
          if (latest_session != display_preparation_e::ready) {
            return {latest_session, nullptr};
          }
          const auto input = api.is_input_desktop(desktop);
          if (!input) {
            return {};
          }
          if (*input && !close_requested) {
            const auto close_status = api.close_screen_saver(desktop);
            if (close_status != display_preparation_e::ready) {
              return {close_status, nullptr};
            }
            close_requested = true;
          }
        } else {
          api.failure("unrecognized input desktop; display configuration was not attempted");
          return {};
        }

        guard.reset();
        if (attempt < 30) {
          api.wait_for_desktop();
        }
      }
      api.failure("input desktop did not become ready within the screen saver transition deadline");
      return {};
    }
  }  // namespace detail
}  // namespace platf
