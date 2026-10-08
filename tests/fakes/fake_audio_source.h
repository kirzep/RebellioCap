#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "engine/recorder_engine.h"

namespace rebelliocap::testing {

class EmptyAudioPipeline final : public IEngineAudioPipeline {
 public:
  Result<std::vector<EncodedPacket>> next(std::chrono::milliseconds) override {
    return Result<std::vector<EncodedPacket>>::success({});
  }

  Result<std::vector<EncodedPacket>> flush() override {
    return Result<std::vector<EncodedPacket>>::success({});
  }
};

class FiniteAudioPipeline final : public IEngineAudioPipeline {
 public:
  explicit FiniteAudioPipeline(std::size_t packet_count) : packet_count_(packet_count) {}

  Result<std::vector<EncodedPacket>> next(std::chrono::milliseconds timeout) override {
    std::unique_lock lock(mutex_);
    if (next_thread_ == std::thread::id{}) {
      next_thread_ = std::this_thread::get_id();
    }
    if (emitted_ >= packet_count_) {
      condition_.wait_for(lock, timeout);
      return Result<std::vector<EncodedPacket>>::success({});
    }
    const auto index = emitted_++;
    lock.unlock();
    condition_.notify_all();
    EncodedPacket packet{.stream = StreamKind::SystemAudio,
                         .epoch = 1,
                         .pts = static_cast<QpcTicks>(index) * 600,
                         .dts = static_cast<QpcTicks>(index) * 600,
                         .duration = 600,
                         .keyframe = false,
                         .payload = std::make_shared<std::vector<std::byte>>(1)};
    return Result<std::vector<EncodedPacket>>::success({std::move(packet)});
  }

  Result<std::vector<EncodedPacket>> flush() override {
    flushed_on_next_thread_ = std::this_thread::get_id() == next_thread_;
    return Result<std::vector<EncodedPacket>>::success({});
  }

  bool flushed_on_next_thread() const noexcept { return flushed_on_next_thread_; }

  bool wait_for_packets(std::size_t count, std::chrono::seconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [&] { return emitted_ >= count; });
  }

 private:
  std::size_t packet_count_;
  std::size_t emitted_{0};
  std::mutex mutex_;
  std::condition_variable condition_;
  std::thread::id next_thread_{};
  std::atomic_bool flushed_on_next_thread_{false};
};

}  // namespace rebelliocap::testing
