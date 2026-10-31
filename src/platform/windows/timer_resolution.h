#pragma once

#include <Windows.h>
#include <timeapi.h>

namespace rebelliocap {

// The high-resolution presentation timer does not change DXGI or condition
// variable timeout granularity. Without a process-local request, short worker
// retries and acquisition waits can be rounded up by a 15.6 ms system tick.
// Pair the request with active capture so idle sessions restore timer policy.
class CaptureTimerResolution {
 public:
  CaptureTimerResolution() noexcept : active_(timeBeginPeriod(1) == TIMERR_NOERROR) {}
  ~CaptureTimerResolution() { if (active_) timeEndPeriod(1); }
  CaptureTimerResolution(const CaptureTimerResolution&) = delete;
  CaptureTimerResolution& operator=(const CaptureTimerResolution&) = delete;

  [[nodiscard]] bool active() const noexcept { return active_; }

 private:
  bool active_;
};

}  // namespace rebelliocap
