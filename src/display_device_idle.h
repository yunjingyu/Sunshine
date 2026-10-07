/**
 * @file src/display_device_idle.h
 * @brief Track the screen saver request belonging to the current streaming generation.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace display_device::detail {
  /**
   * @brief Cancel old idle requests when a new display configuration is prepared.
   * @note Generation methods are serialized by the display manager mutex; completion runs on its scheduler.
   */
  class idle_screen_saver_t {
  public:
    /** @brief Begin a new connection and cancel the previous idle request. */
    void begin_connection() {
      ++m_generation;
      cancel();
    }

    /** @brief Return the generation captured by each actual streaming session. */
    uint64_t generation() const {
      return m_generation;
    }

    /** @brief Request the screen saver only for the currently ending streaming generation. */
    bool request(uint64_t generation) {
      if (generation != m_generation) {
        return false;
      }
      m_pending = true;
      return true;
    }

    /** @brief Cancel privacy actions during startup, shutdown, or explicit state reset. */
    void cancel() {
      m_pending = false;
    }

    /**
     * @brief Complete a pending idle action, retaining it for retry when native startup fails.
     * @param start Start and verify the configured screen saver on the scheduler worker.
     * @return Whether no further completion attempt is required.
     */
    template<class StartT>
    bool complete(StartT start) {
      if (!m_pending) {
        return true;
      }
      if (!start()) {
        return false;
      }
      m_pending = false;
      return true;
    }

  private:
    uint64_t m_generation {0};  ///< Configuration generation shared by simultaneous clients.
    std::atomic<bool> m_pending {false};  ///< A last disconnect awaiting successful restoration.
  };
}  // namespace display_device::detail
