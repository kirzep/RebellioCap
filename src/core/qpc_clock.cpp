#include "core/qpc_clock.h"

#include <Windows.h>

namespace rebelliocap {

QpcClock::QpcClock() noexcept : frequency_(0) {
  LARGE_INTEGER frequency{};
  if (QueryPerformanceFrequency(&frequency) != 0) {
    frequency_ = frequency.QuadPart;
  }
}

QpcTicks QpcClock::now() const noexcept {
  LARGE_INTEGER counter{};
  return QueryPerformanceCounter(&counter) != 0 ? counter.QuadPart : 0;
}

QpcTicks QpcClock::frequency() const noexcept { return frequency_; }

std::chrono::nanoseconds QpcClock::to_duration(QpcTicks ticks) const noexcept {
  if (frequency_ <= 0) {
    return std::chrono::nanoseconds::zero();
  }

  constexpr auto kNanosecondsPerSecond = 1'000'000'000LL;
  const auto nanoseconds = static_cast<QpcTicks>(
      (static_cast<long double>(ticks) * kNanosecondsPerSecond) / frequency_);
  return std::chrono::nanoseconds(nanoseconds);
}

}  // namespace rebelliocap
