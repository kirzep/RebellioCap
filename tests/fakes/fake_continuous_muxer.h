#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include "mux/continuous_muxer.h"
namespace rebelliocap {
class FakeContinuousMuxer final : public IContinuousMuxer {
 public:
  Result<void> open(const std::vector<StreamDescriptor>&, const std::filesystem::path& path, Container) override {
    if (destination_collisions > 0) {
      --destination_collisions;
      return Result<void>::failure(
          {"mux.destination_exists", "Injected destination collision", {}});
    }
    if (open_error.has_value()) return Result<void>::failure(*open_error);
    {
      std::unique_lock lock(mutex);
      open_entered = true;
      condition.notify_all();
      condition.wait(lock, [&] { return !block_open; });
      destination = path;
      opened = true;
      ++open_count;
    }
    condition.notify_all();
    return Result<void>::success();
  }
  Result<void> write(const EncodedPacket&) override {
    std::unique_lock lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return !blocked; });
    if (fail) return Result<void>::failure({"mux.injected", "Injected disk failure", {}});
    ++packets;
    return Result<void>::success();
  }
  Result<std::filesystem::path> finalize() override { finished = true; return Result<std::filesystem::path>::success(destination); }
  void abort() noexcept override { aborted = true; }
  void release() { std::scoped_lock lock(mutex); blocked = false; condition.notify_all(); }
  void release_open() { std::scoped_lock lock(mutex); block_open = false; condition.notify_all(); }
  bool wait_entered() { std::unique_lock lock(mutex); return condition.wait_for(lock, std::chrono::seconds(5), [&]{ return entered; }); }
  bool wait_open_entered() {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(5), [&] { return open_entered; });
  }
  bool wait_opened(std::size_t count = 1) {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return open_count >= count;
    });
  }
  std::mutex mutex;
  std::condition_variable condition;
  std::optional<Error> open_error;
  std::size_t destination_collisions{0};
  bool block_open{false}, open_entered{false}, blocked{false}, entered{false},
      fail{false}, opened{false}, finished{false}, aborted{false};
  std::size_t packets{0};
  std::size_t open_count{0};
  std::filesystem::path destination;
};
}
