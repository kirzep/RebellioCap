#include "engine/continuous_recording.h"

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace rebelliocap {
namespace {

Error recording_error(std::string code, std::string message) {
  return {.code = std::move(code), .message = std::move(message), .hresult = std::nullopt};
}

}  // namespace

struct ContinuousRecording::Impl {
  Impl(IContinuousMuxer& value, std::vector<StreamDescriptor> stream_descriptors,
       std::shared_ptr<PacketMemoryBudget> memory_budget, std::function<void()> changed)
      : muxer(value), descriptors(std::move(stream_descriptors)), budget(std::move(memory_budget)), state_changed(std::move(changed)) {}

  IContinuousMuxer& muxer;
  std::vector<StreamDescriptor> descriptors;
  std::shared_ptr<PacketMemoryBudget> budget;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::deque<EncodedPacket> queue;
  std::jthread writer;
  bool accepting{false};
  bool stopping{false};
  bool waiting_for_keyframe{false};
  std::optional<Error> failure;
  std::atomic<std::uint64_t> written{0};
  std::atomic<std::uint64_t> failures{0};
  std::atomic<std::uint64_t> accepted{0};
  std::string recovery_path;
  std::function<void()> state_changed;
  void notify_state_changed() noexcept { try { if(state_changed)state_changed(); } catch (...) {} }

  void abort_with_recovery() noexcept {
    muxer.abort();
    try {
      const auto path = muxer.recovery_path();
      if (!path) return;
      const auto utf8 = path->u8string();
      std::scoped_lock lock(mutex);
      recovery_path.assign(utf8.begin(), utf8.end());
      if (failure) failure->message += " Partial recording retained at: " + recovery_path;
    } catch (...) {
      // Retained media must survive even if diagnostic allocation fails.
    }
    notify_state_changed();
  }

  void latch_failure(Error error) {
    std::unique_lock lock(mutex);
    if (!failure.has_value()) {
      failure = std::move(error);
      ++failures;
    }
    accepting = false;
    stopping = true;
    queue.clear();
    condition.notify_all();
    lock.unlock(); notify_state_changed();
  }

  void run() noexcept {
    for (;;) {
      std::optional<EncodedPacket> packet;
      {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this] { return stopping || !queue.empty(); });
        if (failure.has_value()) break;
        if (!queue.empty()) {
          packet.emplace(std::move(queue.front()));
          queue.pop_front();
        } else if (stopping) {
          break;
        }
      }
      if (!packet.has_value()) continue;
      auto result = muxer.write(*packet);
      if (!result.is_success()) {
        latch_failure(result.error());
        break;
      }
      ++written;
    }

    std::optional<Error> current_failure;
    {
      std::scoped_lock lock(mutex);
      current_failure = failure;
    }
    if (current_failure.has_value()) {
      abort_with_recovery();
      return;
    }
    if (written == 0) {
      latch_failure(recording_error(
          "recording.no_media", "No usable media arrived before recording stopped."));
      abort_with_recovery();
      return;
    }
    auto finalized = muxer.finalize();
    if (!finalized.is_success()) {
      latch_failure(finalized.error());
      abort_with_recovery();
    }
  }
};

ContinuousRecording::ContinuousRecording(
    IContinuousMuxer& muxer, std::vector<StreamDescriptor> descriptors,
    std::shared_ptr<PacketMemoryBudget> budget, std::function<void()> state_changed)
    : implementation_(std::make_unique<Impl>(muxer, std::move(descriptors), std::move(budget), std::move(state_changed))) {}

ContinuousRecording::~ContinuousRecording() { static_cast<void>(stop()); }

Result<void> ContinuousRecording::start(
    const std::filesystem::path& destination, Container container) {
  auto& state = *implementation_;
  {
    std::scoped_lock lock(state.mutex);
    if (state.accepting || state.writer.joinable()) {
      return Result<void>::failure(recording_error(
          "recording.already_active", "Continuous recording is already active."));
    }
    state.queue.clear();
    state.failure.reset();
    state.stopping = false;
    state.written = 0;
    state.waiting_for_keyframe = std::any_of(
        state.descriptors.begin(), state.descriptors.end(),
        [](const auto& descriptor) { return descriptor.kind == StreamKind::Video; });
  }

  auto opened = state.muxer.open(state.descriptors, destination, container);
  if (!opened.is_success()) return opened;
  {
    std::scoped_lock lock(state.mutex);
    state.accepting = true;
  }
  state.writer = std::jthread([&state] { state.run(); });
  return Result<void>::success();
}

Result<void> ContinuousRecording::accept(EncodedPacket packet) {
  auto& state = *implementation_;
  std::unique_lock lock(state.mutex);
  if (state.failure.has_value()) return Result<void>::failure(*state.failure);
  if (!state.accepting) {
    return Result<void>::failure(recording_error(
        "recording.not_active", "Continuous recording is not active."));
  }
  // A recording toggled mid-GOP cannot decode dependent video frames. Audio
  // arriving before the first random-access frame is omitted with those frames.
  if (state.waiting_for_keyframe) {
    if (packet.stream != StreamKind::Video || !packet.keyframe) {
      return Result<void>::success();
    }
    state.waiting_for_keyframe = false;
  }
  auto tracked = state.budget->track(packet.payload);
  if (!tracked.is_success() || state.queue.size() >= kPacketCapacity) {
    state.failure = !tracked.is_success()
        ? recording_error("recording.memory_budget_exceeded", "Continuous recording stopped because the shared encoded-data memory budget is full. The disk may be too slow.")
        : recording_error("recording.queue_full", "The bounded continuous recording queue is full.");
    ++state.failures;
    state.accepting = false;
    state.stopping = true;
    state.queue.clear();
    state.condition.notify_all();
    const auto failure = *state.failure;
    lock.unlock(); state.notify_state_changed();
    return Result<void>::failure(failure);
  }
  packet.payload = std::move(tracked).value();
  state.queue.push_back(std::move(packet));
  ++state.accepted;
  state.condition.notify_one();
  return Result<void>::success();
}

Result<void> ContinuousRecording::stop() {
  auto& state = *implementation_;
  {
    std::scoped_lock lock(state.mutex);
    if (!state.writer.joinable()) {
      if (state.failure.has_value()) return Result<void>::failure(*state.failure);
      state.accepting = false;
      return Result<void>::success();
    }
    state.accepting = false;
    state.stopping = true;
  }
  state.condition.notify_all();
  state.writer.join();

  std::scoped_lock lock(state.mutex);
  if (state.failure.has_value()) return Result<void>::failure(*state.failure);
  return Result<void>::success();
}

bool ContinuousRecording::active() const {
  const auto& state = *implementation_;
  std::scoped_lock lock(state.mutex);
  return state.accepting && !state.failure.has_value();
}

std::uint64_t ContinuousRecording::packets_written() const noexcept {
  return implementation_->written.load();
}

std::uint64_t ContinuousRecording::failures() const noexcept {
  return implementation_->failures.load();
}

std::uint64_t ContinuousRecording::packets_accepted() const noexcept {
  return implementation_->accepted.load();
}

std::string ContinuousRecording::recovery_path() const {
  const auto& state = *implementation_;
  std::scoped_lock lock(state.mutex);
  return state.recovery_path;
}

}  // namespace rebelliocap
