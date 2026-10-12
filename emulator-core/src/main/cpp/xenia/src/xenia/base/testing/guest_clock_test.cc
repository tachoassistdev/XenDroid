/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "xenia/base/clock.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::base::test {

namespace {
constexpr uint64_t kGuestFrequency = 50000000;

double MeasureRatio(uint32_t reads) {
  // Guest ticks elapsed per host tick, scaled to the nominal ratio (1.0).
  uint64_t host_begin = Clock::QueryHostTickCount();
  uint64_t guest_begin = Clock::QueryGuestTickCount();
  for (uint32_t i = 0; i < reads; ++i) {
    Clock::QueryGuestTickCount();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  uint64_t guest_end = Clock::QueryGuestTickCount();
  uint64_t host_end = Clock::QueryHostTickCount();
  double guest_seconds = double(guest_end - guest_begin) / kGuestFrequency;
  double host_seconds =
      double(host_end - host_begin) / Clock::QueryHostTickFrequency();
  return guest_seconds / host_seconds;
}
}  // namespace

TEST_CASE("Guest clock runs at the guest frequency", "[guest_clock]") {
  Clock::set_guest_time_scalar(1.0);
  Clock::set_guest_tick_frequency(kGuestFrequency);
  // Frequent reads must not slow the clock down (the old accumulation lost a
  // fraction of a tick on each one).
  REQUIRE(MeasureRatio(200000) == Approx(1.0).epsilon(0.002));
  REQUIRE(MeasureRatio(0) == Approx(1.0).epsilon(0.002));
}

TEST_CASE("Guest clock is monotonic and continuous across scalar changes",
          "[guest_clock]") {
  Clock::set_guest_time_scalar(1.0);
  Clock::set_guest_tick_frequency(kGuestFrequency);
  uint64_t previous = Clock::QueryGuestTickCount();
  for (int i = 0; i < 100000; ++i) {
    uint64_t now = Clock::QueryGuestTickCount();
    REQUIRE(now >= previous);
    previous = now;
  }
  uint64_t before = Clock::QueryGuestTickCount();
  Clock::set_guest_time_scalar(2.0);
  uint64_t after = Clock::QueryGuestTickCount();
  REQUIRE(after >= before);
  // No jump: well under 50ms of guest time passes across the switch.
  REQUIRE(after - before < kGuestFrequency / 20);
  REQUIRE(MeasureRatio(0) == Approx(2.0).epsilon(0.01));
  before = Clock::QueryGuestTickCount();
  Clock::set_guest_time_scalar(1.0);
  after = Clock::QueryGuestTickCount();
  REQUIRE(after >= before);
  REQUIRE(after - before < kGuestFrequency / 20);
}

TEST_CASE("Guest clock reads are monotonic per thread under contention",
          "[guest_clock]") {
  Clock::set_guest_time_scalar(1.0);
  Clock::set_guest_tick_frequency(kGuestFrequency);
  std::atomic<bool> go{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&]() {
      while (!go.load()) {
      }
      uint64_t previous = Clock::QueryGuestTickCount();
      for (int i = 0; i < 50000; ++i) {
        uint64_t now = Clock::QueryGuestTickCount();
        if (now < previous) {
          failures.fetch_add(1);
        }
        previous = now;
      }
    });
  }
  std::thread scalar_writer([&]() {
    while (!go.load()) {
    }
    for (int i = 0; i < 50; ++i) {
      Clock::set_guest_time_scalar(i & 1 ? 2.0 : 1.0);
    }
    Clock::set_guest_time_scalar(1.0);
  });
  go.store(true);
  for (auto& thread : threads) {
    thread.join();
  }
  scalar_writer.join();
  REQUIRE(failures.load() == 0);
}

}  // namespace xe::base::test
