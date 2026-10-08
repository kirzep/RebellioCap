#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "engine/recorder_engine.h"

namespace rebelliocap::testing {

class FakeVideoEncoder final : public IEngineVideoPipeline {
 public:
  Result<void> resume() override {
    std::scoped_lock lock(mutex_);
    tick_thread_ = std::this_thread::get_id();
    return Result<void>::success();
  }
  Result<std::vector<EncodedPacket>> tick(QpcTicks pts, bool force_keyframe) override {
    {
      std::scoped_lock lock(mutex_);
      if (tick_thread_ == std::thread::id{}) {
        tick_thread_ = std::this_thread::get_id();
      }
      timestamps_.push_back(pts);
      forced_keyframes_.push_back(force_keyframe);
    }
    condition_.notify_all();
    EncodedPacket packet{.stream = StreamKind::Video,
                         .epoch = 1,
                         .pts = pts,
                         .dts = pts,
                         .duration = 1,
                         .keyframe = force_keyframe,
                         .payload = std::make_shared<std::vector<std::byte>>(1)};
    return Result<std::vector<EncodedPacket>>::success({std::move(packet)});
  }

  Result<std::vector<EncodedPacket>> flush() override {
    flushed_on_tick_thread_ = std::this_thread::get_id() == tick_thread_;
    flushed_ = true;
    return Result<std::vector<EncodedPacket>>::success({});
  }

  bool wait_for_frames(std::size_t count, std::chrono::seconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [&] { return timestamps_.size() >= count; });
  }

  std::vector<QpcTicks> timestamps() const {
    std::scoped_lock lock(mutex_);
    return timestamps_;
  }

  std::vector<bool> forced_keyframes() const {
    std::scoped_lock lock(mutex_);
    return forced_keyframes_;
  }

  bool flushed() const noexcept { return flushed_; }
  bool flushed_on_tick_thread() const noexcept { return flushed_on_tick_thread_; }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<QpcTicks> timestamps_;
  std::vector<bool> forced_keyframes_;
  std::atomic_bool flushed_{false};
  std::atomic_bool flushed_on_tick_thread_{false};
  std::thread::id tick_thread_{};
};

}  // namespace rebelliocap::testing
