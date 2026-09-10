// SPDX-License-Identifier: GPL-3.0-only

/**
 * @file src/platform/linux/input/inputtino_grab.h
 * @brief RAII wrapper for exclusive evdev grab (EVIOCGRAB) used for
 *        same-seat input isolation hardening.
 */
#pragma once

#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <vector>

// local includes
#include "src/logging.h"

namespace platf {

  /**
   * @brief RAII guard that takes an exclusive EVIOCGRAB on a set of virtual
   *        input device fds for the duration of a streaming session.
   *
   * When seat isolation is active (see inputtino_seat::seat_isolation_active()),
   * this guard issues `ioctl(fd, EVIOCGRAB, 1)` on every supplied fd on
   * construction and `ioctl(fd, EVIOCGRAB, 0)` on destruction or `release()`.
   *
   * @par Teardown invariant
   * The guard MUST be destroyed BEFORE the inputtino device that owns the fd,
   * so the grab is released while the uinput device still exists. Store guards
   * as members that are declared AFTER the device members in the parent struct,
   * so C++ member destruction order (reverse of declaration) releases grabs
   * first. For gamepads stored in joypad_state, attach the guard to the state
   * so it releases when the gamepad is freed.
   *
   * @par Failure mode
   * If EVIOCGRAB fails on an individual fd (e.g. the device is already grabbed
   * by another process), the failure is logged as a warning, that fd is closed,
   * and the session continues with the remaining fds — isolation is best-effort
   * for same-seat, while udev ID_SEAT routing (Layer A) is the hard guarantee.
   */
  class evdev_grab_guard_t {
  public:
    /**
     * @brief Function-object test seam for ::ioctl.
     *
     * Unit tests can substitute a mock implementation to verify that
     * EVIOCGRAB 1/0 are issued in the correct order and on the correct fds
     * without needing a real input device.
     */
    using ioctl_fn_t = std::function<int(int fd, unsigned long request, int arg)>;

    /**
     * @brief Default constructor — creates an empty guard owning no fds.
     */
    evdev_grab_guard_t() = default;

    /**
     * @brief Construct a grab guard over a vector of evdev file descriptors.
     *
     * @param fds The open `/dev/input/event*` file descriptors to grab.
     *            The guard takes ownership of every supplied fd.
     * @param seat_active Whether seat isolation is active. When `false`, all
     *                    supplied fds are `::close()`d immediately and the guard
     *                    remains empty.
     * @param ioctl_fn Function object used to issue ioctls. Defaults to `::ioctl`.
     */
    evdev_grab_guard_t(std::vector<int> fds, bool seat_active, ioctl_fn_t ioctl_fn = [](int fd, unsigned long req, int arg) {
      return ::ioctl(fd, req, arg);
    }):
        ioctl_fn_(std::move(ioctl_fn)) {
      if (!seat_active) {
        for (int fd : fds) {
          if (fd >= 0) {
            ::close(fd);
          }
        }
        return;
      }

      using namespace std::literals;
      for (int fd : fds) {
        if (fd < 0) {
          BOOST_LOG(warning) << "inputtino_grab: invalid fd supplied; skipping"sv;
          continue;
        }

        if (ioctl_fn_(fd, EVIOCGRAB, 1) == 0) {
          fds_.push_back(fd);
        } else {
          const int err = errno;
          BOOST_LOG(warning) << "inputtino_grab: EVIOCGRAB failed on fd " << fd << " (errno=" << err << ") - continuing without exclusive grab"sv;
          ::close(fd);
        }
      }
    }

    /// Non-copyable (owns ioctl state on raw fds).
    evdev_grab_guard_t(const evdev_grab_guard_t &) = delete;
    evdev_grab_guard_t &operator=(const evdev_grab_guard_t &) = delete;

    /**
     * @brief Move-constructible (transfers grab ownership).
     *
     * @param other The guard to move from. After the move, `other` holds no
     *              fds and its destructor is a no-op.
     */
    evdev_grab_guard_t(evdev_grab_guard_t &&other) noexcept:
        fds_(std::move(other.fds_)),
        ioctl_fn_(std::move(other.ioctl_fn_)) {
      other.fds_.clear();
    }

    /**
     * @brief Move-assignable. Releases any previously held state first.
     *
     * @param other The guard to move from. After the move, `other` holds no
     *              fds and its destructor is a no-op.
     * @return *this.
     */
    evdev_grab_guard_t &
      operator=(evdev_grab_guard_t &&other) noexcept {
      if (this != &other) {
        release();
        fds_ = std::move(other.fds_);
        ioctl_fn_ = std::move(other.ioctl_fn_);
      }
      return *this;
    }

    /**
     * @brief Destructor — releases the grab if it was acquired.
     *
     * The grab is released BEFORE the inputtino device destroys the uinput
     * object, per the teardown invariant documented on the class.
     */
    ~evdev_grab_guard_t() {
      release();
    }

    /**
     * @brief Release every held EVIOCGRAB and close all owned fds.
     *
     * This is idempotent: calling it more than once has no further effect.
     */
    void
      release() {
      for (int fd : fds_) {
        ioctl_fn_(fd, EVIOCGRAB, 0);
      }
      for (int fd : fds_) {
        ::close(fd);
      }
      fds_.clear();
    }

    /**
     * @brief Whether this guard is currently holding at least one exclusive grab.
     * @return True if EVIOCGRAB was successfully acquired on at least one fd.
     */
    bool
      is_grabbed() const {
      return !fds_.empty();
    }

  private:
    std::vector<int> fds_;  ///< Fds on which EVIOCGRAB succeeded (ownership).
    ioctl_fn_t ioctl_fn_;  ///< Test seam / ioctl implementation.
  };

}  // namespace platf
