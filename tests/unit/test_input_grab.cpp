// SPDX-License-Identifier: GPL-3.0-only

/**
 * @file tests/unit/test_input_grab.cpp
 * @brief Unit tests for the evdev EVIOCGRAB RAII guard.
 */
#include "../tests_common.h"

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>
#include <vector>

// local includes
#include "src/platform/linux/input/inputtino_grab.h"

namespace {

  /**
   * @brief Open a file descriptor that can be closed by the guard.
   *
   * Uses the read end of a pipe. The write end is closed immediately so the
   * fd is a valid, unique file descriptor.
   */
  int open_test_fd() {
    int fds[2];
    if (pipe(fds) != 0) {
      return -1;
    }
    ::close(fds[1]);
    return fds[0];
  }

  bool fd_is_open(int fd) {
    if (fd < 0) {
      return false;
    }
    return ::fcntl(fd, F_GETFD) >= 0 || errno != EBADF;
  }

  TEST(InputGrabGuard, NoOpWhenSeatNotActive) {
    int fd = open_test_fd();
    ASSERT_GE(fd, 0);

    {
      platf::evdev_grab_guard_t guard {{fd}, false};
      EXPECT_FALSE(guard.is_grabbed());
    }

    // The fd was closed immediately by the no-op constructor.
    EXPECT_FALSE(fd_is_open(fd));
  }

  TEST(InputGrabGuard, ActiveMockGrabRelease) {
    int fd = open_test_fd();
    ASSERT_GE(fd, 0);

    std::vector<std::tuple<int, unsigned long, int>> calls;

    auto mock_ioctl = [&calls](int fd_, unsigned long request, int arg) -> int {
      calls.emplace_back(fd_, request, arg);
      return 0;
    };

    {
      platf::evdev_grab_guard_t guard {{fd}, true, mock_ioctl};
      EXPECT_TRUE(guard.is_grabbed());

      // One successful EVIOCGRAB 1 call.
      ASSERT_EQ(calls.size(), 1u);
      EXPECT_EQ(std::get<0>(calls[0]), fd);
      EXPECT_EQ(std::get<1>(calls[0]), EVIOCGRAB);
      EXPECT_EQ(std::get<2>(calls[0]), 1);
      calls.clear();

      guard.release();

      // release() issues EVIOCGRAB 0 then close().
      ASSERT_EQ(calls.size(), 1u);
      EXPECT_EQ(std::get<0>(calls[0]), fd);
      EXPECT_EQ(std::get<1>(calls[0]), EVIOCGRAB);
      EXPECT_EQ(std::get<2>(calls[0]), 0);
    }

    EXPECT_FALSE(fd_is_open(fd));
  }

  TEST(InputGrabGuard, MoveConstructionTransfersOwnership) {
    int fd = open_test_fd();
    ASSERT_GE(fd, 0);

    auto mock_ioctl = [](int, unsigned long, int) {
      return 0;
    };

    platf::evdev_grab_guard_t guard1 {{fd}, true, mock_ioctl};
    EXPECT_TRUE(guard1.is_grabbed());

    platf::evdev_grab_guard_t guard2 {std::move(guard1)};
    EXPECT_FALSE(guard1.is_grabbed());
    EXPECT_TRUE(guard2.is_grabbed());

    // guard1 destructor must not release the fd.
    guard2.release();
    EXPECT_FALSE(guard2.is_grabbed());
    EXPECT_FALSE(fd_is_open(fd));
  }

  TEST(InputGrabGuard, MoveAssignmentTransfersAndReleasesPrevious) {
    int fd1 = open_test_fd();
    int fd2 = open_test_fd();
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);

    std::vector<std::tuple<int, unsigned long, int>> calls;
    auto mock_ioctl = [&calls](int fd_, unsigned long request, int arg) -> int {
      calls.emplace_back(fd_, request, arg);
      return 0;
    };

    platf::evdev_grab_guard_t guard1 {{fd1}, true, mock_ioctl};
    platf::evdev_grab_guard_t guard2 {{fd2}, true, mock_ioctl};
    EXPECT_TRUE(guard1.is_grabbed());
    EXPECT_TRUE(guard2.is_grabbed());

    calls.clear();
    guard2 = std::move(guard1);

    // guard2 released fd2 (EVIOCGRAB 0) before taking fd1.
    EXPECT_FALSE(guard1.is_grabbed());
    EXPECT_TRUE(guard2.is_grabbed());

    bool released_fd2 = false;
    for (const auto &[fd, request, arg] : calls) {
      if (fd == fd2 && request == EVIOCGRAB && arg == 0) {
        released_fd2 = true;
      }
      // fd1 was already grabbed by guard1; moving must not re-grab it.
      EXPECT_FALSE(fd == fd1 && request == EVIOCGRAB && arg == 1);
    }
    EXPECT_TRUE(released_fd2);

    // fd2 was closed by guard2 during assignment.
    EXPECT_FALSE(fd_is_open(fd2));

    guard2.release();
    EXPECT_FALSE(fd_is_open(fd1));
  }

  TEST(InputGrabGuard, PartialFailureClosesFailingFdAndContinues) {
    int fd1 = open_test_fd();
    int fd2 = open_test_fd();
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);

    auto mock_ioctl = [fd1](int fd_, unsigned long request, int) -> int {
      if (fd_ == fd1 && request == EVIOCGRAB) {
        return -1;
      }
      return 0;
    };

    platf::evdev_grab_guard_t guard {{fd1, fd2}, true, mock_ioctl};
    EXPECT_TRUE(guard.is_grabbed());

    // fd1 failed and was closed; fd2 was grabbed.
    EXPECT_FALSE(fd_is_open(fd1));
    EXPECT_TRUE(fd_is_open(fd2));

    guard.release();
    EXPECT_FALSE(fd_is_open(fd2));
  }

  TEST(InputGrabGuard, InvalidFdIsSkippedAndWarned) {
    int fd = open_test_fd();
    ASSERT_GE(fd, 0);

    auto mock_ioctl = [](int, unsigned long, int) {
      return 0;
    };

    platf::evdev_grab_guard_t guard {{-1, fd}, true, mock_ioctl};
    EXPECT_TRUE(guard.is_grabbed());

    // The valid fd was grabbed; the invalid fd was skipped without grabbing.
    guard.release();
    EXPECT_FALSE(fd_is_open(fd));
  }

  TEST(InputGrabGuard, DefaultIoctlFailsGracefully) {
    int fd = open_test_fd();
    ASSERT_GE(fd, 0);

    // Use the real ::ioctl default; a pipe fd does not support EVIOCGRAB,
    // so the grab fails, the fd is closed, and the guard remains empty.
    platf::evdev_grab_guard_t guard {{fd}, true};
    EXPECT_FALSE(guard.is_grabbed());
    EXPECT_FALSE(fd_is_open(fd));
  }

}  // namespace
