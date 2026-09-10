// SPDX-License-Identifier: GPL-3.0-only

/**
 * @file tests/unit/test_input_common.cpp
 * @brief Unit tests for inputtino_common.h seat-isolation helpers.
 *
 * Covers the changed lines in src/platform/linux/input/inputtino_common.h:
 *   - event_nodes()
 *   - open_event_nodes() (both overloads, incl. seat_active gating)
 *   - inputtino_name_for_seat()
 *   - input_raw_t / client_input_raw_t constructor seat-wiring
 *
 * Uses the same environment seams as test_input_seat.cpp:
 *   SOLARFLARE_TEST_UDEV_RULES_DIR, SOLARFLARE_TEST_SYSFS_ROOT, XDG_SEAT,
 *   config::input.input_seat.
 *
 * Virtual-device creation (inputtino::Mouse::create etc.) requires /dev/uinput.
 * When /dev/uinput is not accessible, the success branches of the constructors
 * (the ``if (mouse)`` / ``if (keyboard)`` assign_device_to_seat calls) are not
 * reachable; the tests still exercise the no-seat and failure paths and skip
 * the uinput-dependent assertions gracefully.
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
#include <filesystem>
#include <unistd.h>
#include <variant>

// local includes
#include "src/config.h"
#include "src/platform/linux/input/inputtino_common.h"
#include "src/platform/linux/input/inputtino_seat.h"

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

  /**
   * @brief Try to find a /dev/input/event* node openable with O_RDWR.
   *
   * @return The node path, or an empty string if none is openable.
   */
  std::string
    find_rdwr_event_node() {
    namespace fs = std::filesystem;
    const fs::path input_dir = "/dev/input";
    std::error_code ec;
    if (!fs::exists(input_dir, ec)) {
      return {};
    }
    for (const auto &entry : fs::directory_iterator(input_dir, ec)) {
      const std::string filename = entry.path().filename().string();
      if (filename.rfind("event", 0) != 0) {
        continue;
      }
      int fd = ::open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
      if (fd >= 0) {
        ::close(fd);
        return entry.path().string();
      }
    }
    return {};
  }

  class InputCommonTest: public ::testing::Test {
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
    const char *saved_xdg_seat = nullptr;
    const char *saved_udev_rules_dir = nullptr;
    const char *saved_sysfs_root = nullptr;

    void
      SetUp() override {
      saved_input_seat = config::input.input_seat;
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

  // ---------------------------------------------------------------- event_nodes

  TEST_F(InputCommonTest, EventNodesEmptyInput) {
    auto result = platf::event_nodes({});
    EXPECT_TRUE(result.empty());
  }

  TEST_F(InputCommonTest, EventNodesFiltersToEventPathsOnly) {
    std::vector<std::string> nodes = {
      "/dev/input/js0",
      "/dev/input/mouse0",
      "/dev/input/event3",
      "/dev/input/event7",
      "/dev/input/by-path/platform-foo-event-mouse",
    };
    auto result = platf::event_nodes(nodes);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], "/dev/input/event3");
    EXPECT_EQ(result[1], "/dev/input/event7");
  }

  TEST_F(InputCommonTest, EventNodesAllEventPaths) {
    std::vector<std::string> nodes = {"/dev/input/event0", "/dev/input/event1"};
    auto result = platf::event_nodes(nodes);
    ASSERT_EQ(result.size(), 2u);
  }

  TEST_F(InputCommonTest, EventNodesNoEventPaths) {
    std::vector<std::string> nodes = {"/dev/input/js0", "/dev/input/mouse0"};
    auto result = platf::event_nodes(nodes);
    EXPECT_TRUE(result.empty());
  }

  // -------------------------------------------------------- open_event_nodes (1-arg)

  TEST_F(InputCommonTest, OpenEventNodesEmptyInput) {
    auto fds = platf::open_event_nodes({});
    EXPECT_TRUE(fds.empty());
  }

  TEST_F(InputCommonTest, OpenEventNodesSkipsNonExistentNode) {
    auto fds = platf::open_event_nodes({"/dev/input/event999"});
    EXPECT_TRUE(fds.empty());
  }

  TEST_F(InputCommonTest, OpenEventNodesSkipsNonEventPaths) {
    auto fds = platf::open_event_nodes({"/dev/input/js0"});
    EXPECT_TRUE(fds.empty());
  }

  TEST_F(InputCommonTest, OpenEventNodesOpensRealEventNode) {
    const std::string node = find_rdwr_event_node();
    if (node.empty()) {
      GTEST_SKIP() << "No /dev/input/event* node openable with O_RDWR; skipping success-path test";
    }
    auto fds = platf::open_event_nodes({node});
    ASSERT_EQ(fds.size(), 1u);
    EXPECT_GE(fds[0], 0);
    for (int fd : fds) {
      ::close(fd);
    }
  }

  // ------------------------------------------------------ open_event_nodes (seat-gated)

  TEST_F(InputCommonTest, OpenEventNodesSeatGatedReturnsEmptyWhenInactive) {
    auto fds = platf::open_event_nodes({"/dev/input/event0"}, false);
    EXPECT_TRUE(fds.empty());
  }

  TEST_F(InputCommonTest, OpenEventNodesSeatGatedDelegatesWhenActive) {
    // With a non-existent path, the delegated call still returns empty.
    auto fds = platf::open_event_nodes({"/dev/input/event999"}, true);
    EXPECT_TRUE(fds.empty());
  }

  TEST_F(InputCommonTest, OpenEventNodesSeatGatedEmptyInputActive) {
    auto fds = platf::open_event_nodes({}, true);
    EXPECT_TRUE(fds.empty());
  }

  TEST_F(InputCommonTest, OpenEventNodesSeatGatedOpensRealEventNode) {
    const std::string node = find_rdwr_event_node();
    if (node.empty()) {
      GTEST_SKIP() << "No /dev/input/event* node openable with O_RDWR; skipping success-path test";
    }
    auto fds = platf::open_event_nodes({node}, true);
    ASSERT_EQ(fds.size(), 1u);
    EXPECT_GE(fds[0], 0);
    for (int fd : fds) {
      ::close(fd);
    }
  }

  // ---------------------------------------------------------- inputtino_name_for_seat

  TEST_F(InputCommonTest, NameForSeatEmptyReturnsBase) {
    config::input.input_seat.clear();
    set_xdg_seat(nullptr);
    EXPECT_EQ(platf::inputtino_name_for_seat("Mouse passthrough"), "Mouse passthrough");
  }

  TEST_F(InputCommonTest, NameForSeatSeat0ReturnsBase) {
    config::input.input_seat = "seat0";
    set_xdg_seat(nullptr);
    EXPECT_EQ(platf::inputtino_name_for_seat("Mouse passthrough"), "Mouse passthrough");
  }

  TEST_F(InputCommonTest, NameForSeatNonDefaultAppendsSuffix) {
    config::input.input_seat = "seat1";
    set_xdg_seat(nullptr);
    EXPECT_EQ(platf::inputtino_name_for_seat("Mouse passthrough"), "Mouse passthrough (seat1)");
  }

  TEST_F(InputCommonTest, NameForSeatEnvFallback) {
    config::input.input_seat.clear();
    set_xdg_seat("seat2");
    EXPECT_EQ(platf::inputtino_name_for_seat("Keyboard"), "Keyboard (seat2)");
  }

  // ----------------------------------------------------- input_raw_t constructor

  TEST_F(InputCommonTest, InputRawConstructsWithoutSeat) {
    config::input.input_seat.clear();
    set_xdg_seat(nullptr);
    platf::input_raw_t raw;
    // Without seat, grab guards are no-ops (empty).
    EXPECT_FALSE(raw.mouse_grab.is_grabbed());
    EXPECT_FALSE(raw.keyboard_grab.is_grabbed());
  }

  TEST_F(InputCommonTest, InputRawConstructorNamesCarrySeatSuffix) {
    config::input.input_seat = "seat1";
    set_xdg_seat(nullptr);
    platf::input_raw_t raw;
    EXPECT_NE(raw.mouse_name.find("seat1"), std::string::npos);
    EXPECT_NE(raw.keyboard_name.find("seat1"), std::string::npos);
  }

  TEST_F(InputCommonTest, InputRawConstructorSeatActiveCreatesDevices) {
    if (!uinput_available()) {
      GTEST_SKIP() << "/dev/uinput not accessible; virtual device creation will fail";
    }
    config::input.input_seat = "seat1";
    set_xdg_seat(nullptr);
    platf::input_raw_t raw;
    // With uinput, mouse/keyboard creation succeeds and the if(mouse)/if(keyboard)
    // assign_device_to_seat branches are entered.
    EXPECT_TRUE(raw.mouse);
    EXPECT_TRUE(raw.keyboard);
  }

  // ------------------------------------------------- client_input_raw_t constructor

  TEST_F(InputCommonTest, ClientInputRawConstructsWithoutSeat) {
    config::input.input_seat.clear();
    set_xdg_seat(nullptr);
    platf::input_raw_t *global = new platf::input_raw_t();
    platf::input_t input(global);
    {
      platf::client_input_raw_t client(input);
      EXPECT_FALSE(client.touch_grab.is_grabbed());
      EXPECT_FALSE(client.pen_grab.is_grabbed());
    }
  }

  TEST_F(InputCommonTest, ClientInputRawConstructorNamesCarrySeatSuffix) {
    config::input.input_seat = "seat1";
    set_xdg_seat(nullptr);
    platf::input_raw_t *global = new platf::input_raw_t();
    platf::input_t input(global);
    {
      platf::client_input_raw_t client(input);
      EXPECT_NE(client.touch_name.find("seat1"), std::string::npos);
      EXPECT_NE(client.pen_name.find("seat1"), std::string::npos);
    }
  }

  TEST_F(InputCommonTest, ClientInputRawConstructorSeatActiveCreatesDevices) {
    if (!uinput_available()) {
      GTEST_SKIP() << "/dev/uinput not accessible; virtual device creation will fail";
    }
    config::input.input_seat = "seat1";
    set_xdg_seat(nullptr);
    platf::input_raw_t *global = new platf::input_raw_t();
    platf::input_t input(global);
    {
      platf::client_input_raw_t client(input);
      EXPECT_TRUE(client.touch);
      EXPECT_TRUE(client.pen);
    }
  }

}  // namespace
