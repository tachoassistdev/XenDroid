/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/clock.h"

#include <atomic>
#include <mutex>

#include "xenia/base/assert.h"
#include "xenia/base/math.h"
#include "xenia/base/mutex.h"

#if defined(_WIN32)

#include "xenia/base/platform_win.h"

#endif

DEFINE_bool(clock_no_scaling, false,
            "Disable scaling code. Time management and locking is bypassed. "
            "Guest system time is directly pulled from host.",
            "CPU");
DEFINE_bool(clock_source_raw, false,
            "On x64, Use the RDTSC instruction as the time source. Requires "
            "invariant TSC. "
            "On a64, Use the CNTVCT_EL0 register as the time source",
            "CPU");

namespace xe {

// Time scalar applied to all time operations.
double guest_time_scalar_ = 1.0;
// Tick frequency of guest.
uint64_t guest_tick_frequency_ = Clock::host_tick_frequency_platform();
// Base FILETIME of the guest system from app start.
uint64_t guest_system_time_base_ = Clock::QueryHostSystemTime();
// Combined time and frequency ratio between host and guest.
// Split in numerator (first) and denominator (second).
// Computed by RecomputeGuestTickScalar.
std::pair<uint64_t, uint64_t> guest_tick_ratio_ = std::make_pair(1, 1);

// Guest ticks are a linear function of host ticks between ratio changes:
//   guest = anchor_guest + (host - anchor_host) * ratio
// Each read evaluates that from the anchor instead of accumulating a truncated
// delta under a lock. The old accumulation dropped up to (den - 1) / den of a
// tick on every call, so the guest clock ran slow in proportion to how often
// the title read it, and every mftb took a process-wide mutex.
// The anchor only moves when the ratio changes (startup, time scalar), under
// tick_mutex_; readers take a seqlock snapshot and never block.
struct GuestClockAnchor {
  std::atomic<uint64_t> host;
  std::atomic<uint64_t> guest;
  std::atomic<uint64_t> numerator;
  std::atomic<uint64_t> denominator;
};
static GuestClockAnchor guest_clock_anchor_ = {
    Clock::QueryHostTickCount(), 0, 1, 1};
// Odd while the anchor is being rewritten.
static std::atomic<uint32_t> guest_clock_anchor_seq_{0};

// Most recent guest tick count handed out, for GetGuestTickCountPointer.
uint64_t last_guest_tick_count_ = 0;

using tick_mutex_type = std::mutex;

// Serializes anchor and ratio writers. Readers do not take it.
static tick_mutex_type tick_mutex_;

// floor(delta * num / den) without the 64-bit overflow of the product.
static inline uint64_t ScaleTicks(uint64_t delta, uint64_t num,
                                  uint64_t den) {
  return (delta / den) * num + (delta % den) * num / den;
}

static inline uint64_t GuestTicksAtHost(uint64_t host_tick_count) {
  uint64_t anchor_host, anchor_guest, num, den;
  uint32_t seq;
  do {
    seq = guest_clock_anchor_seq_.load(std::memory_order_acquire);
    anchor_host = guest_clock_anchor_.host.load(std::memory_order_relaxed);
    anchor_guest = guest_clock_anchor_.guest.load(std::memory_order_relaxed);
    num = guest_clock_anchor_.numerator.load(std::memory_order_relaxed);
    den = guest_clock_anchor_.denominator.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
  } while ((seq & 1) ||
           seq != guest_clock_anchor_seq_.load(std::memory_order_relaxed));
  uint64_t host_tick_delta =
      host_tick_count > anchor_host ? host_tick_count - anchor_host : 0;
  return anchor_guest + ScaleTicks(host_tick_delta, num, den);
}

// Caller holds tick_mutex_. Re-anchors at the current host time so the guest
// clock stays continuous across the ratio change.
static void SetGuestTickRatioLocked(std::pair<uint64_t, uint64_t> ratio) {
  uint32_t seq = guest_clock_anchor_seq_.load(std::memory_order_relaxed);
  guest_clock_anchor_seq_.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  // Sampled only now that readers are held off: any value a reader computed
  // with the old anchor used a host time no later than this one, so the guest
  // clock cannot step backwards when the new ratio is smaller.
  uint64_t host_tick_count = Clock::QueryHostTickCount();
  uint64_t anchor_host =
      guest_clock_anchor_.host.load(std::memory_order_relaxed);
  uint64_t host_tick_delta =
      host_tick_count > anchor_host ? host_tick_count - anchor_host : 0;
  uint64_t guest_tick_count =
      guest_clock_anchor_.guest.load(std::memory_order_relaxed) +
      ScaleTicks(host_tick_delta,
                 guest_clock_anchor_.numerator.load(std::memory_order_relaxed),
                 guest_clock_anchor_.denominator.load(
                     std::memory_order_relaxed));
  guest_clock_anchor_.host.store(host_tick_count, std::memory_order_relaxed);
  guest_clock_anchor_.guest.store(guest_tick_count, std::memory_order_relaxed);
  guest_clock_anchor_.numerator.store(ratio.first, std::memory_order_relaxed);
  guest_clock_anchor_.denominator.store(ratio.second,
                                        std::memory_order_relaxed);
  guest_clock_anchor_seq_.store(seq + 2, std::memory_order_release);
  guest_tick_ratio_ = ratio;
}

void RecomputeGuestTickScalar() {
  // Create a rational number with numerator (first) and denominator (second)
  auto frac =
      std::make_pair(guest_tick_frequency_, Clock::QueryHostTickFrequency());
  // Doing it this way ensures we don't mess up our frequency scaling and
  // precisely controls the precision the guest_time_scalar_ can have.
  if (guest_time_scalar_ > 1.0) {
    frac.first *= static_cast<uint64_t>(guest_time_scalar_ * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
  }
  // Keep this a rational calculation and reduce the fraction
  reduce_fraction(frac);

  std::lock_guard<tick_mutex_type> lock(tick_mutex_);
  SetGuestTickRatioLocked(frac);
}

// Current guest tick count. Lock-free.
uint64_t UpdateGuestClock() {
  uint64_t host_tick_count = Clock::QueryHostTickCount();

  if (cvars::clock_no_scaling) {
    // Nothing to update, calculate on the fly
    return host_tick_count * guest_tick_ratio_.first / guest_tick_ratio_.second;
  }

  uint64_t guest_tick_count = GuestTicksAtHost(host_tick_count);
  // Only consumed by the x64 inline mftb path; a racing older value is
  // harmless there, so no CAS loop to contend on.
  std::atomic_ref<uint64_t> last(last_guest_tick_count_);
  if (guest_tick_count > last.load(std::memory_order_relaxed)) {
    last.store(guest_tick_count, std::memory_order_relaxed);
  }
  return guest_tick_count;
}

// Offset of the current guest system file time relative to the guest base time.
inline uint64_t QueryGuestSystemTimeOffset() {
  if (cvars::clock_no_scaling) {
    return Clock::QueryHostSystemTime() - guest_system_time_base_;
  }

  auto guest_tick_count = UpdateGuestClock();

  uint64_t numerator = 10000000;  // 100ns/10MHz resolution
  uint64_t denominator = guest_tick_frequency_;
  reduce_fraction(numerator, denominator);

  return guest_tick_count * numerator / denominator;
}
uint64_t Clock::QueryHostTickFrequency() {
#if XE_CLOCK_RAW_AVAILABLE
  if (cvars::clock_source_raw) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
uint64_t Clock::QueryHostTickCount() {
#if XE_CLOCK_RAW_AVAILABLE
  if (cvars::clock_source_raw) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

double Clock::guest_time_scalar() { return guest_time_scalar_; }

void Clock::set_guest_time_scalar(double scalar) {
  if (cvars::clock_no_scaling) {
    return;
  }

  guest_time_scalar_ = scalar;
  RecomputeGuestTickScalar();
}

std::pair<uint64_t, uint64_t> Clock::guest_tick_ratio() {
  std::lock_guard<tick_mutex_type> lock(tick_mutex_);
  return guest_tick_ratio_;
}

uint64_t Clock::guest_tick_frequency() { return guest_tick_frequency_; }

void Clock::set_guest_tick_frequency(uint64_t frequency) {
  guest_tick_frequency_ = frequency;
  RecomputeGuestTickScalar();
}

uint64_t Clock::guest_system_time_base() { return guest_system_time_base_; }

void Clock::set_guest_system_time_base(uint64_t time_base) {
  guest_system_time_base_ = time_base;
}

uint64_t Clock::QueryGuestTickCount() {
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

uint64_t* Clock::GetGuestTickCountPointer() { return &last_guest_tick_count_; }
uint64_t Clock::QueryGuestSystemTime() {
  if (cvars::clock_no_scaling) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return guest_system_time_base_ + guest_system_time_offset;
}

uint64_t Clock::QueryGuestInterruptTime() {
  return Clock::QueryHostInterruptTime();
}

uint32_t Clock::QueryGuestUptimeMillis() {
  return static_cast<uint32_t>(
      std::min<uint64_t>(QueryGuestSystemTimeOffset() / 10000,
                         std::numeric_limits<uint32_t>::max()));
}

void Clock::SetGuestSystemTime(uint64_t system_time) {
  if (cvars::clock_no_scaling) {
    // Time is fixed to host time.
    return;
  }

  // Query the filetime offset to calculate a new base time.
  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  guest_system_time_base_ = system_time - guest_system_time_offset;
}

uint32_t Clock::ScaleGuestDurationMillis(uint32_t guest_ms) {
  if (cvars::clock_no_scaling) {
    return guest_ms;
  }

  constexpr uint64_t max = std::numeric_limits<uint32_t>::max();

  if (guest_ms >= max) {
    return max;
  } else if (!guest_ms) {
    return 0;
  }
  uint64_t scaled_ms = static_cast<uint64_t>(
      (static_cast<uint64_t>(guest_ms) * guest_time_scalar_));
  return static_cast<uint32_t>(std::min(scaled_ms, max));
}

int64_t Clock::ScaleGuestDurationFileTime(int64_t guest_file_time) {
  if (cvars::clock_no_scaling) {
    return static_cast<uint64_t>(guest_file_time);
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    // Absolute time.
    uint64_t guest_time = Clock::QueryGuestSystemTime();
    int64_t relative_time = guest_file_time - static_cast<int64_t>(guest_time);
    int64_t scaled_time =
        static_cast<int64_t>(relative_time * guest_time_scalar_);
    return static_cast<int64_t>(guest_time) + scaled_time;
  } else {
    // Relative time.
    uint64_t scaled_file_time = static_cast<uint64_t>(
        (static_cast<uint64_t>(guest_file_time) * guest_time_scalar_));
    // TODO(benvanik): check for overflow?
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(int32_t* tv_sec, int32_t* tv_usec) {
  if (cvars::clock_no_scaling) {
    return;
  }

  uint64_t scaled_sec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_sec) *
                                              guest_time_scalar_);
  uint64_t scaled_usec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_usec) *
                                               guest_time_scalar_);
  if (scaled_usec > std::numeric_limits<uint32_t>::max()) {
    uint64_t overflow_sec = scaled_usec / 1000000;
    scaled_usec -= overflow_sec * 1000000;
    scaled_sec += overflow_sec;
  }
  *tv_sec = int32_t(scaled_sec);
  *tv_usec = int32_t(scaled_usec);
}

}  // namespace xe
