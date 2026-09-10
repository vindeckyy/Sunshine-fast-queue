// SPDX-License-Identifier: GPL-3.0-only

/**
 * @file src/platform/linux/input/inputtino_common.h
 * @brief Declarations for inputtino common input handling.
 */
#pragma once

// lib includes
#include <boost/locale.hpp>
#include <fcntl.h>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>
#include <unistd.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/linux/input/inputtino_grab.h"
#include "src/platform/linux/input/inputtino_seat.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf {

  /**
   * @brief Append the resolved seat suffix to a base device name.
   *
   * If no non-default seat is configured, the base name is returned unchanged.
   *
   * @param base_name The device name without any seat suffix.
   * @return The seat-aware device name (e.g. "Mouse passthrough (seat1)").
   */
  inline std::string inputtino_name_for_seat(std::string_view base_name) {
    auto seat_id = inputtino_seat::get_target_seat();
    if (seat_id.empty() || seat_id == "seat0") {
      return std::string(base_name);
    }

    std::string name;
    name.reserve(base_name.size() + seat_id.size() + 3);
    name.append(base_name);
    name.append(" (");
    name.append(seat_id);
    name.push_back(')');
    return name;
  }

  /**
   * @brief Filter a device node list down to only `/dev/input/event*` paths.
   *
   * This is used for the EVIOCGRAB layer: only event nodes can be grabbed,
   * while the full node list (including `/dev/input/js*` children) is passed
   * to the seat assignment layer.
   *
   * @param nodes The full node list returned by `inputtino::get_nodes()`.
   * @return A vector containing only entries that start with `/dev/input/event`.
   */
  inline std::vector<std::string> event_nodes(const std::vector<std::string> &nodes) {
    std::vector<std::string> result;
    for (const auto &node : nodes) {
      if (node.starts_with("/dev/input/event"sv)) {
        result.push_back(node);
      }
    }
    return result;
  }

  /**
   * @brief Open every `/dev/input/event*` node in a device node list.
   *
   * @param nodes The full node list returned by `inputtino::get_nodes()`.
   * @return A vector of open file descriptors (owned by the caller). Failed
   *         opens are logged and skipped.
   */
  inline std::vector<int> open_event_nodes(const std::vector<std::string> &nodes) {
    std::vector<int> fds;
    for (const auto &node : event_nodes(nodes)) {
      int fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
      if (fd < 0) {
        const int err = errno;
        BOOST_LOG(warning) << "inputtino_common: failed to open event node " << node << " (errno=" << err << ')';
        continue;
      }
      fds.push_back(fd);
    }
    return fds;
  }

  /**
   * @brief Seat-gated variant of open_event_nodes() for the grab guards.
   *
   * When seat isolation is inactive (the default no-seat path), this returns an
   * empty vector WITHOUT issuing any `::open()` syscall. The grab guard then
   * receives no fds and stays empty, so no open/close syscalls happen at all
   * in that path. When seat isolation IS active, behavior is identical to
   * open_event_nodes().
   *
   * @param nodes The full node list returned by `inputtino::get_nodes()`.
   * @param seat_active Whether seat isolation is active. When `false`, no node
   *                    is opened and an empty vector is returned.
   * @return A vector of open file descriptors (owned by the caller), or empty
   *         when seat isolation is inactive.
   */
  inline std::vector<int> open_event_nodes(const std::vector<std::string> &nodes, bool seat_active) {
    if (!seat_active) {
      return {};
    }
    return open_event_nodes(nodes);
  }

  using joypads_t = std::variant<inputtino::XboxOneJoypad, inputtino::SwitchJoypad, inputtino::PS5Joypad>;

  struct joypad_state {
    std::unique_ptr<joypads_t> joypad;
    gamepad_feedback_msg_t last_rumble;
    gamepad_feedback_msg_t last_rgb_led;
    evdev_grab_guard_t grab_guard;  ///< RAII EVIOCGRAB for this gamepad's event nodes.
  };

  struct input_raw_t {
    input_raw_t():
        mouse_name(inputtino_name_for_seat("Mouse passthrough"sv)),
        mouse(inputtino::Mouse::create({
          .name = mouse_name,
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })),
        keyboard_name(inputtino_name_for_seat("Keyboard passthrough"sv)),
        keyboard(inputtino::Keyboard::create({
          .name = keyboard_name,
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })),
        gamepads(MAX_GAMEPADS) {
      const auto seat = inputtino_seat::get_target_seat();
      if (!seat.empty()) {
        BOOST_LOG(info) << "inputtino: target seat is '" << seat << "'"sv;
      }
      const bool seat_active = inputtino_seat::seat_isolation_active();

      if (mouse) {
        inputtino_seat::assign_device_to_seat(mouse_name, (*mouse).get_nodes(), seat);
      }
      if (keyboard) {
        inputtino_seat::assign_device_to_seat(keyboard_name, (*keyboard).get_nodes(), seat);
      }

      mouse_grab = evdev_grab_guard_t {open_event_nodes(mouse ? (*mouse).get_nodes() : std::vector<std::string> {}, seat_active), seat_active};
      keyboard_grab = evdev_grab_guard_t {open_event_nodes(keyboard ? (*keyboard).get_nodes() : std::vector<std::string> {}, seat_active), seat_active};

      if (!mouse) {
        BOOST_LOG(warning) << "Unable to create virtual mouse: " << mouse.getErrorMessage();
      }
      if (!keyboard) {
        BOOST_LOG(warning) << "Unable to create virtual keyboard: " << keyboard.getErrorMessage();
      }
    }

    ~input_raw_t() = default;

    std::string mouse_name;  ///< Seat-aware mouse device name.
    // All devices are wrapped in Result because it might be that we aren't able to create them (ex: udev permission denied)
    inputtino::Result<inputtino::Mouse> mouse;
    evdev_grab_guard_t mouse_grab;  ///< RAII EVIOCGRAB for mouse event nodes.

    std::string keyboard_name;  ///< Seat-aware keyboard device name.
    inputtino::Result<inputtino::Keyboard> keyboard;
    evdev_grab_guard_t keyboard_grab;  ///< RAII EVIOCGRAB for keyboard event nodes.

    /**
     * A list of gamepads that are currently connected.
     * The pointer is shared because that state will be shared with background threads that deal with rumble and LED
     */
    std::vector<std::shared_ptr<joypad_state>> gamepads;
  };

  struct client_input_raw_t: public client_input_t {
    client_input_raw_t(input_t &input):
        touch_name(inputtino_name_for_seat("Touch passthrough"sv)),
        touch(inputtino::TouchScreen::create({
          .name = touch_name,
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })),
        pen_name(inputtino_name_for_seat("Pen passthrough"sv)),
        pen(inputtino::PenTablet::create({
          .name = pen_name,
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })) {
      global = (input_raw_t *) input.get();

      const auto seat = inputtino_seat::get_target_seat();
      const bool seat_active = inputtino_seat::seat_isolation_active();

      if (touch) {
        inputtino_seat::assign_device_to_seat(touch_name, (*touch).get_nodes(), seat);
      }
      if (pen) {
        inputtino_seat::assign_device_to_seat(pen_name, (*pen).get_nodes(), seat);
      }

      touch_grab = evdev_grab_guard_t {open_event_nodes(touch ? (*touch).get_nodes() : std::vector<std::string> {}, seat_active), seat_active};
      pen_grab = evdev_grab_guard_t {open_event_nodes(pen ? (*pen).get_nodes() : std::vector<std::string> {}, seat_active), seat_active};

      if (!touch) {
        BOOST_LOG(warning) << "Unable to create virtual touch screen: " << touch.getErrorMessage();
      }
      if (!pen) {
        BOOST_LOG(warning) << "Unable to create virtual pen tablet: " << pen.getErrorMessage();
      }
    }

    input_raw_t *global;  ///< Global mouse/keyboard/gamepad state shared across clients.

    std::string touch_name;  ///< Seat-aware touch device name.
    // Device state and handles for pen and touch input must be stored in the per-client
    // input context, because each connected client may be sending their own independent
    // pen/touch events. To maintain separation, we expose separate pen and touch devices
    // for each client.
    inputtino::Result<inputtino::TouchScreen> touch;
    evdev_grab_guard_t touch_grab;  ///< RAII EVIOCGRAB for touch event nodes.

    std::string pen_name;  ///< Seat-aware pen device name.
    inputtino::Result<inputtino::PenTablet> pen;
    evdev_grab_guard_t pen_grab;  ///< RAII EVIOCGRAB for pen event nodes.
  };

  inline float deg2rad(float degree) {
    return degree * (M_PI / 180.f);
  }
}  // namespace platf
