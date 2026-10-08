#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>

#include "engine/recorder_engine.h"
#include "fakes/fake_hotkey_source.h"

namespace rebelliocap::testing {

class VirtualEngineClock final : public IEngineClock {
 public:
  explicit VirtualEngineClock(QpcTicks frequency) : frequency_(frequency) {}

  QpcTicks now() const noexcept override { return now_.load(); }
  QpcTicks frequency() const noexcept override { return frequency_; }

  bool wait_until(QpcTicks deadline, std::stop_token stop) override {
    std::unique_lock lock(mutex_);
    std::stop_callback wake(stop, [this] { condition_.notify_all(); });
    condition_.wait(lock, [&] { return stop.stop_requested() || deadline <= limit_; });
    if (stop.stop_requested()) {
      return false;
    }
    now_.store((std::max)(now_.load(), deadline));
    return true;
  }

  void elapse(QpcTicks ticks) {
    const auto current = now_.fetch_add(ticks) + ticks;
    {
      std::scoped_lock lock(mutex_);
      limit_ = (std::max)(limit_, current);
    }
    condition_.notify_all();
  }

  void advance_through(QpcTicks deadline) {
    {
      std::scoped_lock lock(mutex_);
      limit_ = deadline;
    }
    condition_.notify_all();
  }

 private:
  QpcTicks frequency_;
  std::atomic<QpcTicks> now_{0};
  std::mutex mutex_;
  std::condition_variable condition_;
  QpcTicks limit_{0};
};

}  // namespace rebelliocap::testing
