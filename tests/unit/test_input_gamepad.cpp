// SPDX-License-Identifier: GPL-3.0-only

/**
 * @file tests/unit/test_input_gamepad.cpp
 * @brief Unit tests for inputtino_gamepad.cpp seat-isolation changes.
 *
 * Covers the changed lines in src/platform/linux/input/inputtino_gamepad.cpp:
 *   - create_xbox_one / create_switch / create_ds5 (now take a seat-aware name)
 *   - alloc() seat-wiring (get_target_seat, seat_isolation_active,
 *     assign_device_to_seat, open_event_nodes, grab_guard) for all three
 *     controller types
 *   - free() grab_guard release
 *   - supported_gamepads() base-name probing
 *
 * Virtual-device creation requires /dev/uinput.  When /dev/uinput is not
 * accessible, the success branches of alloc (which contain the seat-wiring)
 * are not reachable; the tests exercise the failure paths and skip the
 * uinput-dependent assertions gracefully.
 */
// inputtino/input.hpp must be included BEFORE tests_common.h (which pulls in
// Limelight.h).  Limelight.h defines several macros (SPECIAL_FLAG,
// PADDLE1_FLAG, …, MISC_FLAG) that collide with enum values in inputtino's
// CONTROLLER_BTN enum, causing parse failures.  Including input.hpp first
// lets the enum parse before the macros are defined.  This mirrors the
// include order in inputtino_gamepad.cpp.
// clang-format off
#include <inputtino/input.hpp>
// clang-format on
#include "../tests_common.h"

#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <unistd.h>
#include <variant>

// local includes
#include "src/config.h"
#include "src/globals.h"
#include "src/platform/common.h"
#include "src/platform/linux/input/inputtino_common.h"
#include "src/platform/linux/input/inputtino_gamepad.h"
#include "src/platform/linux/input/inputtino_seat.h"
#include "src/thread_safe.h"

namespace {

  /// @brief Probe whether /dev/uinput is accessible for virtual device creation.
  bool
    uinput_available() {
    int fd = ::open("/dev/uinput", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      return false;
    }
    ::close(fd);
    return true;
  }

  /// @brief Create a feedback queue suitable for platf::gamepad::alloc().
  platf::feedback_queue_t
    make_feedback_queue() {
    auto mail = std::make_shared<safe::mail_raw_t>();
    return mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
  }

  class InputGamepadTest: public ::testing::Test {
  protected:
    static void
      SetUpTestSuite() {
      ::setenv("XDG_SEAT", "seat0", 1);
      ::setenv("SOLARFLARE_TEST_UDEV_RULES_DIR", "/tmp", 1);
      ::setenv("SOLARFLARE_TEST_SYSFS_ROOT", "/tmp", 1);
    }

    static void
      TearDownTestSuite() {
      ::unsetenv("XDG_SEAT");
      ::unsetenv("SOLARFLARE_TEST_UDEV_RULES_DIR");
      ::unsetenv("SOLARFLARE_TEST_SYSFS_ROOT");
    }

    std::string saved_input_seat;
    std::string saved_gamepad;
    bool saved_motion_as_ds4 = false;
    bool saved_touchpad_as_ds4 = false;
    bool saved_ds5_randomize_mac = false;
    const char *saved_xdg_seat = nullptr;
    const char *saved_udev_rules_dir = nullptr;
    const char *saved_sysfs_root = nullptr;

    void
      SetUp() override {
      saved_input_seat = config::input.input_seat;
      saved_gamepad = config::input.gamepad;
      saved_motion_as_ds4 = config::input.motion_as_ds4;
      saved_touchpad_as_ds4 = config::input.touchpad_as_ds4;
      saved_ds5_randomize_mac = config::input.ds5_inputtino_randomize_mac;
      if (const char *seat = std::getenv("XDG_SEAT")) {
        saved_xdg_seat = seat;
      }
      if (const char *dir = std::getenv("SOLARFLARE_TEST_UDEV_RULES_DIR")) {
        saved_udev_rules_dir = dir;
      }
      if (const char *root = std::getenv("SOLARFLARE_TEST_SYSFS_ROOT")) {
        saved_sysfs_root = root;
      }
    }

    void
      TearDown() override {
      config::input.input_seat = saved_input_seat;
      config::input.gamepad = saved_gamepad;
      config::input.motion_as_ds4 = saved_motion_as_ds4;
      config::input.touchpad_as_ds4 = saved_touchpad_as_ds4;
      config::input.ds5_inputtino_randomize_mac = saved_ds5_randomize_mac;
      restore_env("XDG_SEAT", saved_xdg_seat);
      restore_env("SOLARFLARE_TEST_UDEV_RULES_DIR", saved_udev_rules_dir);
      restore_env("SOLARFLARE_TEST_SYSFS_ROOT", saved_sysfs_root);
    }

    static void
      restore_env(const char *name, const char *saved) {
      if (saved != nullptr) {
        ASSERT_EQ(::setenv(name, saved, 1), 0);
      } else {
        ::unsetenv(name);
      }
    }

    static void
      set_xdg_seat(const char *value) {
      if (value) {
        ASSERT_EQ(::setenv("XDG_SEAT", value, 1), 0);
      } else {
        ASSERT_EQ(::unsetenv("XDG_SEAT"), 0);
      }
    }
  };

  // ----------------------------------------------------------------- alloc / free

  TEST_F(InputGamepadTest, AllocFailsWithoutUinput) {
    if (uinput_available()) {
      GTEST_SKIP() << "/dev/uinput is accessible; failure-path test not applicable";
    }
    config::input.input_seat.clear();
    set_xdg_seat(nullptr);
    config::input.gamepad = "xone";

    platf::input_raw_t raw;
    platf::gamepad_id_t id {.globalIndex = 0, .clientRelativeIndex = 0};
    platf::gamepad_arrival_t metadata {};
    auto feedback_queue = make_feedback_queue();

    EXPECT_EQ(platf::gamepad::alloc(&raw, id, metadata, feedback_queue), -1);
  }

  TEST_F(InputGamepadTest, AllocXboxOneWithSeat) {
    if (!uinput_available()) {
      GTEST_SKIP() << "/dev/uinput not accessible; cannot create virtual gamepad";
    }
    config::input.input_seat = "seat1";
    set_xdg_seat(nullptr);
    config::input.gamepad = "xone";

    platf::input_raw_t raw;
    platf::gamepad_id_t id {.globalIndex = 0, .clientRelativeIndex = 0};
    platf::gamepad_arrival_t metadata {};
    auto feedback_queue = make_feedback_queue();

    EXPECT_EQ(platf::gamepad::alloc(&raw, id, metadata, feedback_queue), 0);
    ASSERT_TRUE(raw.gamepads[0]);
    EXPECT_TRUE(raw.gamepads[0]->joypad);

    platf::gamepad::free(&raw, 0);
    EXPECT_FALSE(raw.gamepads[0]);
  }

  TEST_F(InputGamepadTest, AllocSwitchProWithSeat) {
    if (!uinput_available()) {
      GTEST_SKIP() << "/dev/uinput not accessible; cannot create virtual gamepad";
    }
    config::input.input_seat = "seat1";
    set_xdg_seat(nullptr);
    config::input.gamepad = "switch";

    platf::input_raw_t raw;
    platf::gamepad_id_t id {.globalIndex = 1, .clientRelativeIndex = 0};
    platf::gamepad_arrival_t metadata {};
    auto feedback_queue = make_feedback_queue();

    EXPECT_EQ(platf::gamepad::alloc(&raw, id, metadata, feedback_queue), 0);
    ASSERT_TRUE(raw.gamepads[1]);
    EXPECT_TRUE(raw.gamepads[1]->joypad);

    platf::gamepad::free(&raw, 1);
    EXPECT_FALSE(raw.gamepads[1]);
  }

  TEST_F(InputGamepadTest, AllocDualSenseWithSeat) {
    if (!uinput_available()) {
      GTEST_SKIP() << "/dev/uinput not accessible; cannot create virtual gamepad";
    }
    config::input.input_seat = "seat1";
    set_xdg_seat(nullptr);
    config::input.gamepad = "ds5";
    config::input.ds5_inputtino_randomize_mac = true;

    platf::input_raw_t raw;
    platf::gamepad_id_t id {.globalIndex = 2, .clientRelativeIndex = 0};
    platf::gamepad_arrival_t metadata {};
    auto feedback_queue = make_feedback_queue();

    EXPECT_EQ(platf::gamepad::alloc(&raw, id, metadata, feedback_queue), 0);
    ASSERT_TRUE(raw.gamepads[2]);
    EXPECT_TRUE(raw.gamepads[2]->joypad);

    platf::gamepad::free(&raw, 2);
    EXPECT_FALSE(raw.gamepads[2]);
  }

  TEST_F(InputGamepadTest, AllocAutoSelectsXboxByDefault) {
    if (!uinput_available()) {
      GTEST_SKIP() << "/dev/uinput not accessible; cannot create virtual gamepad";
    }
    config::input.input_seat.clear();
    set_xdg_seat(nullptr);
    config::input.gamepad = "auto";
    config::input.motion_as_ds4 = false;
    config::input.touchpad_as_ds4 = false;

    platf::input_raw_t raw;
    platf::gamepad_id_t id {.globalIndex = 3, .clientRelativeIndex = 0};
    platf::gamepad_arrival_t metadata {.type = 0, .capabilities = 0, .supportedButtons = 0};
    auto feedback_queue = make_feedback_queue();

    EXPECT_EQ(platf::gamepad::alloc(&raw, id, metadata, feedback_queue), 0);
    ASSERT_TRUE(raw.gamepads[3]);

    platf::gamepad::free(&raw, 3);
  }

  // ---------------------------------------------------------- supported_gamepads

  TEST_F(InputGamepadTest, SupportedGamepadsNullInputReturnsStaticList) {
    auto &gps = platf::gamepad::supported_gamepads(nullptr);
    ASSERT_GE(gps.size(), 4u);
    EXPECT_EQ(gps[0].name, "auto");
    EXPECT_TRUE(gps[0].is_enabled);
  }

  TEST_F(InputGamepadTest, SupportedGamepadsProbesDevices) {
    // input_t is a uniq_ptr with a custom operator&(); wrap it in a unique_ptr
    // so we can obtain a platf::input_t* via .get().
    auto input = std::make_unique<platf::input_t>(new platf::input_raw_t());

    auto &gps = platf::gamepad::supported_gamepads(input.get());
    ASSERT_GE(gps.size(), 4u);
    EXPECT_EQ(gps[0].name, "auto");
    // The probe entries may be enabled or disabled depending on uinput access.
    EXPECT_FALSE(gps[1].name.empty());
  }

}  // namespace
