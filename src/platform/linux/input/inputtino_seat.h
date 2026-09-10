// SPDX-License-Identifier: GPL-3.0-only

/**
 * @file src/platform/linux/input/inputtino_seat.h
 * @brief Helpers for multi-seat input device isolation (udev-only).
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace platf::inputtino_seat {

  /**
   * Determine the target seat for the current Sunshine instance.
   *
   * Resolution order: config::input.input_seat (if non-empty) >
   * XDG_SEAT environment variable > empty string.
   *
   * @return The target seat name, or empty if none could be determined.
   */
  std::string get_target_seat();

  /**
   * Check whether seat isolation is active.
   *
   * Isolation is active when the resolved seat is non-empty and not
   * the default seat ("seat0").
   *
   * @return True if seat isolation should be applied.
   */
  bool seat_isolation_active();

  /**
   * Assign a virtual input device to a systemd-logind seat by setting
   * the udev ID_SEAT property.
   *
   * Writes a single transient udev rule to /run/udev/rules.d/ and
   * synthesizes a @c change uevent for every supplied device node.
   * Requires CAP_SYS_ADMIN (already granted to the Sunshine binary).
   *
   * @param device_name The name of the virtual device (as passed to inputtino).
   * @param device_nodes The full node list returned by inputtino::get_nodes()
   *        (e.g. /dev/input/event0 and /dev/input/js0 for gamepads).
   * @param seat The target seat name.
   * @return True after attempting the assignment. Seat routing is best-effort:
   *         individual rule/uevent failures are logged as warnings and the
   *         session continues. False is returned only for an empty or unsafe
   *         seat name, or when the requested seat is "seat0".
   */
  bool assign_device_to_seat(std::string_view device_name, const std::vector<std::string> &device_nodes, std::string_view seat);

}  // namespace platf::inputtino_seat
