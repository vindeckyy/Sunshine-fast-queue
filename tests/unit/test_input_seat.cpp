// SPDX-License-Identifier: GPL-3.0-only

/**
 * @file tests/unit/test_input_seat.cpp
 * @brief Unit tests for input seat helpers and input_seat config parsing.
 */
#include "../tests_common.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

// local includes
#include "src/config.h"
#include "src/platform/linux/input/inputtino_seat.h"

namespace config {
  // Internal helper exercised by the round-trip test.
  void string_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input);
}  // namespace config

namespace {

  class InputSeatConfigTest: public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
      // Pre-populate the environment so the save/restore branches in SetUp and
      // TearDown are exercised. The fixture restores the values at the end.
      ::setenv("XDG_SEAT", "seat0", 1);
      ::setenv("SOLARFLARE_TEST_UDEV_RULES_DIR", "/tmp", 1);
      ::setenv("SOLARFLARE_TEST_SYSFS_ROOT", "/tmp", 1);
    }

    static void TearDownTestSuite() {
      ::unsetenv("XDG_SEAT");
      ::unsetenv("SOLARFLARE_TEST_UDEV_RULES_DIR");
      ::unsetenv("SOLARFLARE_TEST_SYSFS_ROOT");
    }

    std::string saved_input_seat;
    const char *saved_xdg_seat = nullptr;
    const char *saved_udev_rules_dir = nullptr;
    const char *saved_sysfs_root = nullptr;

    void SetUp() override {
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

    void TearDown() override {
      config::input.input_seat = saved_input_seat;

      if (saved_xdg_seat != nullptr) {
        ASSERT_EQ(::setenv("XDG_SEAT", saved_xdg_seat, 1), 0);
      } else {
        ::unsetenv("XDG_SEAT");
      }

      if (saved_udev_rules_dir != nullptr) {
        ASSERT_EQ(::setenv("SOLARFLARE_TEST_UDEV_RULES_DIR", saved_udev_rules_dir, 1), 0);
      } else {
        ::unsetenv("SOLARFLARE_TEST_UDEV_RULES_DIR");
      }

      if (saved_sysfs_root != nullptr) {
        ASSERT_EQ(::setenv("SOLARFLARE_TEST_SYSFS_ROOT", saved_sysfs_root, 1), 0);
      } else {
        ::unsetenv("SOLARFLARE_TEST_SYSFS_ROOT");
      }
    }

    static void set_xdg_seat(const char *value) {
      if (value) {
        ASSERT_EQ(::setenv("XDG_SEAT", value, 1), 0);
      } else {
        ASSERT_EQ(::unsetenv("XDG_SEAT"), 0);
      }
    }
  };

  TEST_F(InputSeatConfigTest, InputSeatDefaultsToEmpty) {
    EXPECT_TRUE(config::input.input_seat.empty());
  }

  TEST_F(InputSeatConfigTest, InputSeatAcceptsSeat1) {
    config::input.input_seat = "seat1";
    EXPECT_EQ(config::input.input_seat, "seat1");
  }

  TEST_F(InputSeatConfigTest, StringFRoundTripsInputSeat) {
    std::unordered_map<std::string, std::string> vars {
      {"input_seat", "seat1"},
    };
    config::input.input_seat.clear();
    config::string_f(vars, "input_seat", config::input.input_seat);
    EXPECT_EQ(config::input.input_seat, "seat1");

    // Missing key leaves the value unchanged.
    vars.clear();
    config::input.input_seat = "seat1";
    config::string_f(vars, "input_seat", config::input.input_seat);
    EXPECT_EQ(config::input.input_seat, "seat1");
  }

  TEST_F(InputSeatConfigTest, GetTargetSeatConfigPrecedesEnvironment) {
    config::input.input_seat = "seat1";
    set_xdg_seat("seat0");
    EXPECT_EQ(platf::inputtino_seat::get_target_seat(), "seat1");
  }

  TEST_F(InputSeatConfigTest, GetTargetSeatFallsBackToXdgSeat) {
    config::input.input_seat.clear();
    set_xdg_seat("seat1");
    EXPECT_EQ(platf::inputtino_seat::get_target_seat(), "seat1");
  }

  TEST_F(InputSeatConfigTest, GetTargetSeatReturnsEmptyWhenNeitherSet) {
    config::input.input_seat.clear();
    set_xdg_seat(nullptr);
    EXPECT_TRUE(platf::inputtino_seat::get_target_seat().empty());
  }

  TEST_F(InputSeatConfigTest, SeatIsolationActiveForEmptyAndSeat0) {
    config::input.input_seat.clear();
    set_xdg_seat(nullptr);
    EXPECT_FALSE(platf::inputtino_seat::seat_isolation_active());

    config::input.input_seat = "seat0";
    EXPECT_FALSE(platf::inputtino_seat::seat_isolation_active());

    config::input.input_seat = "seat1";
    EXPECT_TRUE(platf::inputtino_seat::seat_isolation_active());
  }

  TEST_F(InputSeatConfigTest, AssignDeviceToSeatReturnsFalseForEmptyAndSeat0) {
    EXPECT_FALSE(platf::inputtino_seat::assign_device_to_seat("dev", {}, ""));
    EXPECT_FALSE(platf::inputtino_seat::assign_device_to_seat("dev", {}, "seat0"));
  }

  /**
   * @brief Locate an existing /dev/input/event* node and its canonical sysfs path.
   * @return A pair of {devnode, sysfs_path} if found, otherwise std::nullopt.
   */
  std::optional<std::pair<std::string, std::filesystem::path>> find_input_event_node() {
    namespace fs = std::filesystem;

    const fs::path input_dir = [] {
      if (const char *env = std::getenv("SOLARFLARE_TEST_INPUT_DEV_DIR")) {
        if (env[0] != '\0') {
          return fs::path(env);
        }
      }
      return fs::path("/dev/input");
    }();

    const fs::path sysfs_class_dir = [] {
      if (const char *env = std::getenv("SOLARFLARE_TEST_INPUT_SYSFS_CLASS_DIR")) {
        if (env[0] != '\0') {
          return fs::path(env);
        }
      }
      return fs::path("/sys/class/input");
    }();

    if (!fs::exists(input_dir)) {
      return std::nullopt;
    }

    for (const auto &entry : fs::directory_iterator(input_dir)) {
      const std::string filename = entry.path().filename().string();
      if (filename.rfind("event", 0) == 0) {
        const fs::path sys_link = sysfs_class_dir / filename;
        if (!fs::exists(sys_link)) {
          continue;
        }
        std::error_code ec;
        const fs::path sys_path = fs::canonical(sys_link, ec);
        if (ec) {
          continue;
        }
        return std::make_pair(entry.path().string(), sys_path);
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Build the test sysfs uevent path from a canonical sysfs path and a test root.
   *
   * Mirrors platf::inputtino_seat::map_sysfs_uevent_path.
   */
  std::filesystem::path build_test_uevent_path(const std::filesystem::path &sysfs_path, const std::filesystem::path &test_root) {
    const std::string syspath_str = sysfs_path.string();  // keep the underlying string alive
    std::string_view syspath_sv = syspath_str;
    if (syspath_sv.starts_with("/sys")) {
      syspath_sv.remove_prefix(4);
      if (!syspath_sv.empty() && syspath_sv.front() == '/') {
        syspath_sv.remove_prefix(1);
      }
    }
    return test_root / std::string(syspath_sv) / "uevent";
  }

  TEST_F(InputSeatConfigTest, FindInputEventNodeReturnsNulloptForMissingDirectory) {
    namespace fs = std::filesystem;
    const fs::path missing_dir = fs::temp_directory_path() / ("missing-input-" + std::to_string(::getpid()));

    ASSERT_EQ(::setenv("SOLARFLARE_TEST_INPUT_DEV_DIR", missing_dir.string().c_str(), 1), 0);
    EXPECT_FALSE(find_input_event_node());
    ::unsetenv("SOLARFLARE_TEST_INPUT_DEV_DIR");
  }

  TEST_F(InputSeatConfigTest, FindInputEventNodeReturnsNulloptForEmptyDirectory) {
    namespace fs = std::filesystem;
    const fs::path empty_dir = fs::temp_directory_path() / ("empty-input-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::create_directories(empty_dir, ec);
    ASSERT_FALSE(ec) << "Failed to create empty input test directory";

    ASSERT_EQ(::setenv("SOLARFLARE_TEST_INPUT_DEV_DIR", empty_dir.string().c_str(), 1), 0);
    EXPECT_FALSE(find_input_event_node());
    ::unsetenv("SOLARFLARE_TEST_INPUT_DEV_DIR");
    fs::remove_all(empty_dir, ec);
  }

  TEST_F(InputSeatConfigTest, FindInputEventNodeSkipsEntriesWithoutSysfsLink) {
    namespace fs = std::filesystem;
    const fs::path input_dir = fs::temp_directory_path() / ("fake-input-" + std::to_string(::getpid()));
    const fs::path sysfs_class_dir = fs::temp_directory_path() / ("fake-sysfs-" + std::to_string(::getpid()));

    std::error_code ec;
    fs::create_directories(input_dir, ec);
    ASSERT_FALSE(ec);
    fs::create_directories(sysfs_class_dir, ec);
    ASSERT_FALSE(ec);

    // A fake event node without a matching /sys/class/input link is skipped.
    { std::ofstream ofs(input_dir / "event0"); }

    ASSERT_EQ(::setenv("SOLARFLARE_TEST_INPUT_DEV_DIR", input_dir.string().c_str(), 1), 0);
    ASSERT_EQ(::setenv("SOLARFLARE_TEST_INPUT_SYSFS_CLASS_DIR", sysfs_class_dir.string().c_str(), 1), 0);
    EXPECT_FALSE(find_input_event_node());
    ::unsetenv("SOLARFLARE_TEST_INPUT_DEV_DIR");
    ::unsetenv("SOLARFLARE_TEST_INPUT_SYSFS_CLASS_DIR");
    fs::remove_all(input_dir, ec);
    fs::remove_all(sysfs_class_dir, ec);
  }

  TEST_F(InputSeatConfigTest, AssignDeviceToSeatWritesRuleAndUevent) {
    namespace fs = std::filesystem;

    auto node_opt = find_input_event_node();
    if (!node_opt) {
      GTEST_SKIP() << "No real /dev/input/event* node available; skipping uevent test";
    }

    const auto &[device_node, sysfs_path] = *node_opt;

    const fs::path tmp_root = fs::temp_directory_path() / ("solarflare-seat-test-" + std::to_string(::getpid()));
    const fs::path rules_dir = tmp_root / "rules";
    const fs::path sysfs_root = tmp_root / "sys";

    const fs::path uevent_path = build_test_uevent_path(sysfs_path, sysfs_root);
    std::error_code mkdir_ec;
    fs::create_directories(uevent_path.parent_path(), mkdir_ec);
    ASSERT_FALSE(mkdir_ec) << "Failed to create test sysfs directories: " << uevent_path.parent_path() << " (" << mkdir_ec.message() << ")";

    ASSERT_EQ(::setenv("SOLARFLARE_TEST_UDEV_RULES_DIR", rules_dir.string().c_str(), 1), 0);
    ASSERT_EQ(::setenv("SOLARFLARE_TEST_SYSFS_ROOT", sysfs_root.string().c_str(), 1), 0);

    const bool result = platf::inputtino_seat::assign_device_to_seat("Test device", {device_node}, "seat1");
    EXPECT_TRUE(result);

    // Verify the transient udev rule was written with the correct seat.
    const fs::path rule_file = rules_dir / "99-solarflare-seat.rules";
    ASSERT_TRUE(fs::exists(rule_file)) << "Expected udev rule file at " << rule_file;
    std::ifstream rule_ifs(rule_file);
    const std::string rule_content((std::istreambuf_iterator<char>(rule_ifs)), std::istreambuf_iterator<char>());
    EXPECT_NE(rule_content.find("ATTRS{name}==\"* (seat1)\""), std::string::npos);
    EXPECT_NE(rule_content.find("ENV{ID_SEAT}=\"seat1\""), std::string::npos);

    // Verify a change uevent was synthesized in the test sysfs root.
    ASSERT_TRUE(fs::exists(uevent_path)) << "Expected uevent file at " << uevent_path;
    std::ifstream uevent_ifs(uevent_path);
    const std::string uevent_content((std::istreambuf_iterator<char>(uevent_ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(uevent_content, "change\n");

    // Clean up temp directory is handled by the fixture; explicit unsetenv
    // is kept here to avoid leaking state before TearDown.
    ::unsetenv("SOLARFLARE_TEST_UDEV_RULES_DIR");
    ::unsetenv("SOLARFLARE_TEST_SYSFS_ROOT");
    std::error_code rm_ec;
    fs::remove_all(tmp_root, rm_ec);
  }

  TEST_F(InputSeatConfigTest, AssignDeviceToSeatRejectsUnsafeSeat) {
    EXPECT_FALSE(platf::inputtino_seat::assign_device_to_seat("dev", {}, "seat\"1"));
    EXPECT_FALSE(platf::inputtino_seat::assign_device_to_seat("dev", {}, "seat\n1"));
  }

  TEST_F(InputSeatConfigTest, AssignDeviceToSeatUpdatesStaleRule) {
    namespace fs = std::filesystem;

    auto node_opt = find_input_event_node();
    if (!node_opt) {
      GTEST_SKIP() << "No real /dev/input/event* node available; skipping uevent test";
    }

    const auto &[device_node, sysfs_path] = *node_opt;

    const fs::path tmp_root = fs::temp_directory_path() / ("solarflare-seat-test-" + std::to_string(::getpid()));
    const fs::path rules_dir = tmp_root / "rules";
    const fs::path sysfs_root = tmp_root / "sys";

    const fs::path uevent_path = build_test_uevent_path(sysfs_path, sysfs_root);
    std::error_code mkdir_ec;
    fs::create_directories(uevent_path.parent_path(), mkdir_ec);
    ASSERT_FALSE(mkdir_ec) << "Failed to create test sysfs directories: " << uevent_path.parent_path() << " (" << mkdir_ec.message() << ")";

    // Pre-populate the rules directory with a stale rule.
    fs::create_directories(rules_dir, mkdir_ec);
    ASSERT_FALSE(mkdir_ec) << "Failed to create test rules directory: " << rules_dir;
    {
      std::ofstream ofs(rules_dir / "99-solarflare-seat.rules");
      ofs << "# stale\n";
    }

    ASSERT_EQ(::setenv("SOLARFLARE_TEST_UDEV_RULES_DIR", rules_dir.string().c_str(), 1), 0);
    ASSERT_EQ(::setenv("SOLARFLARE_TEST_SYSFS_ROOT", sysfs_root.string().c_str(), 1), 0);

    const bool result = platf::inputtino_seat::assign_device_to_seat("Test device", {device_node}, "seat1");
    EXPECT_TRUE(result);

    const fs::path rule_file = rules_dir / "99-solarflare-seat.rules";
    std::ifstream rule_ifs(rule_file);
    const std::string rule_content((std::istreambuf_iterator<char>(rule_ifs)), std::istreambuf_iterator<char>());
    EXPECT_NE(rule_content.find("ENV{ID_SEAT}=\"seat1\""), std::string::npos);

    ::unsetenv("SOLARFLARE_TEST_UDEV_RULES_DIR");
    ::unsetenv("SOLARFLARE_TEST_SYSFS_ROOT");
    std::error_code rm_ec;
    fs::remove_all(tmp_root, rm_ec);
  }

  TEST_F(InputSeatConfigTest, AssignDeviceToSeatWarnsAndContinuesOnUnwritableRule) {
    namespace fs = std::filesystem;

    const fs::path tmp_root = fs::temp_directory_path() / ("solarflare-seat-test-" + std::to_string(::getpid()));
    const fs::path fake_rules_dir = tmp_root / "not_a_dir";
    const fs::path sysfs_root = tmp_root / "sys";

    // Make the "rules directory" path a regular file so create_directories/write fail.
    fs::create_directories(tmp_root);
    { std::ofstream ofs(fake_rules_dir); }

    const fs::path uevent_path = sysfs_root / "uevent";
    std::error_code mkdir_ec;
    fs::create_directories(uevent_path.parent_path(), mkdir_ec);
    ASSERT_FALSE(mkdir_ec);

    ASSERT_EQ(::setenv("SOLARFLARE_TEST_UDEV_RULES_DIR", fake_rules_dir.string().c_str(), 1), 0);
    ASSERT_EQ(::setenv("SOLARFLARE_TEST_SYSFS_ROOT", sysfs_root.string().c_str(), 1), 0);

    const bool result = platf::inputtino_seat::assign_device_to_seat("Test device", {"/dev/input/event999"}, "seat1");
    EXPECT_TRUE(result);

    ::unsetenv("SOLARFLARE_TEST_UDEV_RULES_DIR");
    ::unsetenv("SOLARFLARE_TEST_SYSFS_ROOT");
    std::error_code rm_ec;
    fs::remove_all(tmp_root, rm_ec);
  }

  TEST_F(InputSeatConfigTest, AssignDeviceToSeatFallsBackToUdevadm) {
    namespace fs = std::filesystem;

    auto node_opt = find_input_event_node();
    if (!node_opt) {
      GTEST_SKIP() << "No real /dev/input/event* node available; skipping uevent test";
    }

    const auto &[device_node, sysfs_path] = *node_opt;

    const fs::path tmp_root = fs::temp_directory_path() / ("solarflare-seat-test-" + std::to_string(::getpid()));
    const fs::path rules_dir = tmp_root / "rules";
    const fs::path sysfs_root = tmp_root / "no_such_sysfs";  // intentionally absent

    fs::create_directories(rules_dir);

    ASSERT_EQ(::setenv("SOLARFLARE_TEST_UDEV_RULES_DIR", rules_dir.string().c_str(), 1), 0);
    ASSERT_EQ(::setenv("SOLARFLARE_TEST_SYSFS_ROOT", sysfs_root.string().c_str(), 1), 0);

    // Remove udevadm from PATH so the fallback triggers fail predictably and
    // the last-resort subsystem-wide trigger is exercised.
    const char *saved_path = std::getenv("PATH");
    ASSERT_EQ(::setenv("PATH", "/nonexistent", 1), 0);

    const bool result = platf::inputtino_seat::assign_device_to_seat("Test device", {device_node}, "seat1");
    EXPECT_TRUE(result);

    if (saved_path != nullptr) {
      ASSERT_EQ(::setenv("PATH", saved_path, 1), 0);
    } else {
      ::unsetenv("PATH");
    }

    ::unsetenv("SOLARFLARE_TEST_UDEV_RULES_DIR");
    ::unsetenv("SOLARFLARE_TEST_SYSFS_ROOT");
    std::error_code rm_ec;
    fs::remove_all(tmp_root, rm_ec);
  }

  TEST_F(InputSeatConfigTest, AssignDeviceToSeatUsesDefaultsAndFakeUdevadm) {
    namespace fs = std::filesystem;

    auto node_opt = find_input_event_node();
    if (!node_opt) {
      GTEST_SKIP() << "No real /dev/input/event* node available; skipping uevent test";
    }

    const auto &[device_node, sysfs_path] = *node_opt;

    // Create a fake udevadm that always succeeds so the fallback trigger path
    // records a successful uevent without needing real root privileges.
    const fs::path fake_udev_dir = fs::temp_directory_path() / ("fake-udev-" + std::to_string(::getpid()));
    const fs::path fake_udevadm = fake_udev_dir / "udevadm";
    std::error_code mkdir_ec;
    fs::create_directories(fake_udev_dir, mkdir_ec);
    ASSERT_FALSE(mkdir_ec);
    {
      std::ofstream ofs(fake_udevadm);
      ofs << "#!/bin/sh\nexit 0\n";
    }
    ::chmod(fake_udevadm.string().c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

    // Use empty env values so the implementation falls back to the default
    // /run/udev/rules.d and /sys paths (rule write fails without root, then
    // the fake udevadm trigger succeeds).
    ASSERT_EQ(::setenv("SOLARFLARE_TEST_UDEV_RULES_DIR", "", 1), 0);
    ASSERT_EQ(::setenv("SOLARFLARE_TEST_SYSFS_ROOT", "", 1), 0);

    const char *saved_path = std::getenv("PATH");
    const std::string new_path = fake_udev_dir.string() + ":/usr/bin:/bin";
    ASSERT_EQ(::setenv("PATH", new_path.c_str(), 1), 0);

    const bool result = platf::inputtino_seat::assign_device_to_seat("Test device", {device_node}, "seat1");
    EXPECT_TRUE(result);

    if (saved_path != nullptr) {
      ASSERT_EQ(::setenv("PATH", saved_path, 1), 0);
    } else {
      ::unsetenv("PATH");
    }

    std::error_code rm_ec;
    fs::remove_all(fake_udev_dir, rm_ec);
  }

}  // namespace
