#include "engine/continuous_recording.h"
#include "core/pipeline_telemetry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace rebelliocap {
namespace {
Error recording_error(std::string code, std::string message) {
  return {.code = std::move(code), .message = std::move(message), .hresult = std::nullopt};
}
}
struct ContinuousRecording::Impl {
  struct Entry { EncodedPacket packet; QpcTicks enqueued; std::size_t bytes; };
  Impl(IContinuousMuxer& value, std::vector<StreamDescriptor> streams,
       std::shared_ptr<PacketMemoryBudget> memory, std::function<void()> changed,
       ContinuousQueueLimits configured, std::shared_ptr<PipelineTelemetry> diagnostics)
      : muxer(value), descriptors(std::move(streams)), budget(std::move(memory)),
        state_changed(std::move(changed)), limits(configured), telemetry(std::move(diagnostics)) {
    if (!budget) budget = std::make_shared<PacketMemoryBudget>(PacketMemoryBudget::kDefaultLimit);
    limits.hard_bytes = limits.hard_bytes ? (std::min)(limits.hard_bytes, budget->limit()) : budget->limit();
    limits.soft_bytes = limits.soft_bytes ? (std::min)(limits.soft_bytes, limits.hard_bytes) : limits.hard_bytes - limits.hard_bytes / 4;
    limits.packet_capacity = (std::min)(limits.packet_capacity, kPacketCapacity);
    limits.batch_packets = (std::clamp)(limits.batch_packets, std::size_t{1}, std::size_t{32});
    limits.batch_bytes = (std::max)(limits.batch_bytes, std::size_t{1});
  }
  IContinuousMuxer& muxer;
  std::vector<StreamDescriptor> descriptors;
  std::shared_ptr<PacketMemoryBudget> budget;
  std::function<void()> state_changed;
  ContinuousQueueLimits limits;
  std::shared_ptr<PipelineTelemetry> telemetry;
  QpcClock clock;
  // Serializes lifecycle/join; never held by the disk writer or producers.
  std::mutex lifecycle;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::list<Entry> queue;
  std::jthread writer;
  bool accepting{false}, stopping{false}, waiting_for_keyframe{false};
  std::optional<Error> failure;
  std::atomic<std::uint64_t> written{0}, failures{0}, accepted{0};
  std::string recovery_path;
  ContinuousQueueMetrics metrics;
  QpcTicks in_flight_oldest{0};

  void notify_state_changed() noexcept { try { if (state_changed) state_changed(); } catch (...) {} }
  QpcTicks oldest_locked() const {
    return metrics.in_flight_packets ? in_flight_oldest : queue.empty() ? 0 : queue.front().enqueued;
  }
  QpcTicks age_locked(QpcTicks now) const {
    return metrics.in_flight_packets || !queue.empty() ? (std::max)(QpcTicks{0}, now - oldest_locked()) : 0;
  }
  bool age_reached(QpcTicks age, std::chrono::milliseconds threshold) const {
    return threshold.count() > 0 && clock.to_duration(age) >= threshold;
  }
  bool warning_locked(QpcTicks now) {
    const auto bytes = metrics.queued_bytes + metrics.in_flight_bytes;
    const auto age = age_locked(now);
    const bool prior = metrics.warning;
    if (!prior && ((limits.soft_bytes && bytes >= limits.soft_bytes) || age_reached(age, limits.soft_age)))
      metrics.warning = true;
    else if (prior && bytes <= (std::min)(limits.hard_bytes / 2, limits.soft_bytes ? limits.soft_bytes - 1 : limits.hard_bytes / 2) &&
             (limits.soft_age.count() <= 0 || clock.to_duration(age) <= limits.soft_age / 2))
      metrics.warning = false;
    if (prior != metrics.warning) ++metrics.warning_transitions;
    return prior != metrics.warning;
  }
  void fail_locked(Error error) {
    if (!failure) { failure = std::move(error); ++failures; }
    accepting = false; stopping = true;
    queue.clear(); metrics.queued_bytes = 0; metrics.queued_packets = 0;
    condition.notify_all();
  }
  bool health_locked(QpcTicks now) {
    if (!failure && (accepting || stopping) && age_reached(age_locked(now), limits.hard_age)) {
      fail_locked(recording_error("recording.queue_age_exceeded", "Continuous recording stopped because the oldest pending packet exceeded the disk queue age limit."));
      return true;
    }
    return warning_locked(now);
  }
  void latch_failure(Error error) {
    { std::scoped_lock lock(mutex); fail_locked(std::move(error)); }
    notify_state_changed();
  }
  void abort_with_recovery() noexcept {
    muxer.abort();
    try {
      const auto path = muxer.recovery_path();
      if (path) {
        const auto utf8 = path->u8string();
        std::scoped_lock lock(mutex);
        recovery_path.assign(utf8.begin(), utf8.end());
        if (failure) failure->message += " Partial recording retained at: " + recovery_path;
      }
    } catch (...) {}
    notify_state_changed();
  }
  void run() noexcept {
    try {
      for (;;) {
        std::list<Entry> batch;
        {
          std::unique_lock lock(mutex);
          condition.wait(lock, [this] { return stopping || !queue.empty(); });
          static_cast<void>(health_locked(clock.now()));
          if (failure || (queue.empty() && stopping)) break;
          std::size_t bytes = 0;
          auto end = queue.begin();
          std::size_t count = 0;
          while (end != queue.end() && count < limits.batch_packets) {
            if (count && (end->bytes > limits.batch_bytes || bytes > limits.batch_bytes - end->bytes)) break;
            bytes += end->bytes; ++end; ++count;
          }
          batch.splice(batch.end(), queue, queue.begin(), end);
          metrics.queued_bytes -= bytes; metrics.queued_packets -= count;
          metrics.in_flight_bytes = bytes; metrics.in_flight_packets = count;
          in_flight_oldest = batch.front().enqueued;
        }
        while (!batch.empty()) {
          { std::scoped_lock lock(mutex); if (failure) break; }
          if (telemetry && telemetry->enabled()) {
            const auto age = (std::max)(QpcTicks{0}, clock.now()-batch.front().enqueued);
            telemetry->record(PipelineStage::WriterQueueResidence,
                static_cast<std::uint64_t>(static_cast<long double>(age)*1'000'000.0L/clock.frequency()));
          }
          auto result = [&] {
            PipelineTimingScope timing(telemetry.get(), PipelineStage::WriterMuxCpu);
            return muxer.write(batch.front().packet);
          }();
          bool changed = false;
          {
            std::scoped_lock lock(mutex);
            if (!result.is_success()) fail_locked(result.error());
            else ++written;
            // Retain the enqueue timestamp until the completed write has been
            // checked, even while stop() is draining already accepted packets.
            changed = health_locked(clock.now());
            metrics.in_flight_bytes -= batch.front().bytes; --metrics.in_flight_packets;
            batch.pop_front();
            in_flight_oldest = batch.empty() ? 0 : batch.front().enqueued;
            changed |= warning_locked(clock.now());
          }
          if (changed || !result.is_success()) notify_state_changed();
          if (!result.is_success()) break;
        }
        {
          std::scoped_lock lock(mutex);
          batch.clear(); metrics.in_flight_bytes = 0; metrics.in_flight_packets = 0; in_flight_oldest = 0;
          if (failure) break;
        }
      }
      bool failed;
      { std::scoped_lock lock(mutex); failed = failure.has_value(); }
      if (failed) { abort_with_recovery(); return; }
      if (written == 0) {
        latch_failure(recording_error("recording.no_media", "No usable media arrived before recording stopped."));
        abort_with_recovery(); return;
      }
      auto finalized = muxer.finalize();
      if (!finalized.is_success()) { latch_failure(finalized.error()); abort_with_recovery(); }
    } catch (const std::exception& error) {
      { std::scoped_lock lock(mutex); metrics.in_flight_bytes = 0; metrics.in_flight_packets = 0; in_flight_oldest = 0; }
      latch_failure(recording_error("recording.writer_exception", error.what())); abort_with_recovery();
    } catch (...) {
      { std::scoped_lock lock(mutex); metrics.in_flight_bytes = 0; metrics.in_flight_packets = 0; in_flight_oldest = 0; }
      latch_failure(recording_error("recording.writer_exception", "Unexpected continuous writer failure.")); abort_with_recovery();
    }
  }
};
ContinuousRecording::ContinuousRecording(IContinuousMuxer& muxer, std::vector<StreamDescriptor> descriptors,
    std::shared_ptr<PacketMemoryBudget> budget, std::function<void()> changed, ContinuousQueueLimits limits,
    std::shared_ptr<PipelineTelemetry> telemetry)
    : implementation_(std::make_unique<Impl>(muxer, std::move(descriptors), std::move(budget), std::move(changed), limits, std::move(telemetry))) {}
ContinuousRecording::~ContinuousRecording() { static_cast<void>(stop()); }
Result<void> ContinuousRecording::start(const std::filesystem::path& destination, Container container) {
  auto& state = *implementation_;
  std::scoped_lock lifecycle(state.lifecycle);
  {
    std::scoped_lock lock(state.mutex);
    if (state.accepting || state.writer.joinable()) return Result<void>::failure(recording_error("recording.already_active", "Continuous recording is already active."));
    state.queue.clear(); state.failure.reset(); state.stopping = false; state.written = 0;
    state.metrics = {}; state.in_flight_oldest = 0; state.recovery_path.clear();
    state.waiting_for_keyframe = std::any_of(state.descriptors.begin(), state.descriptors.end(), [](const auto& d) { return d.kind == StreamKind::Video; });
  }
  auto opened = state.muxer.open(state.descriptors, destination, container);
  if (!opened.is_success()) return opened;
  try {
    std::scoped_lock lock(state.mutex);
    state.writer = std::jthread([&state] { state.run(); });
    state.accepting = true;
  } catch (const std::exception& error) {
    state.muxer.abort();
    return Result<void>::failure(recording_error("recording.writer_start", error.what()));
  }
  return Result<void>::success();
}
Result<void> ContinuousRecording::accept(EncodedPacket packet) { return accept_batch(std::span<const EncodedPacket>(&packet, 1)); }
Result<void> ContinuousRecording::accept_batch(std::span<const EncodedPacket> packets) {
  auto& state = *implementation_;
  PipelineTimingScope timing(state.telemetry.get(), PipelineStage::QueueAdmissionCpu);
  std::unique_lock lock(state.mutex);
  bool changed = state.health_locked(state.clock.now());
  auto finish = [&](Result<void> result) { lock.unlock(); if (changed) state.notify_state_changed(); return result; };
  if (state.failure) return finish(Result<void>::failure(*state.failure));
  if (!state.accepting) return finish(Result<void>::failure(recording_error("recording.not_active", "Continuous recording is not active.")));
  std::list<Impl::Entry> staged;
  std::size_t bytes = 0;
  bool waiting = state.waiting_for_keyframe;
  const auto now = state.clock.now();
  const auto occupied = state.metrics.queued_bytes + state.metrics.in_flight_bytes;
  std::optional<Error> error;
  try {
    for (const auto& packet : packets) {
      if (waiting) {
        if (packet.stream != StreamKind::Video || !packet.keyframe) continue;
        waiting = false;
      }
      const auto size = packet.payload ? packet.payload->size() : 0;
      if (state.queue.size() + staged.size() >= state.limits.packet_capacity) {
        error = recording_error("recording.queue_full", "The bounded continuous recording queue is full."); break;
      }
      if (size > state.limits.hard_bytes || occupied > state.limits.hard_bytes - size || bytes > state.limits.hard_bytes - size - occupied) {
        error = recording_error("recording.queue_bytes_exceeded", "Continuous recording stopped because the disk queue byte limit is full."); break;
      }
      auto tracked = state.budget->track(packet.payload);
      if (!tracked.is_success()) {
        error = recording_error("recording.memory_budget_exceeded", "Continuous recording stopped because the shared encoded-data memory budget is full. The disk may be too slow."); break;
      }
      auto copy = packet; copy.payload = std::move(tracked).value();
      staged.push_back({std::move(copy), now, size}); bytes += size;
    }
  } catch (const std::exception& exception) { error = recording_error("recording.admission_exception", exception.what()); }
  if (error) {
    state.fail_locked(std::move(*error)); changed = true;
    return finish(Result<void>::failure(*state.failure));
  }
  const bool wake = state.queue.empty() && !staged.empty();
  state.accepted += staged.size(); state.metrics.queued_packets += staged.size(); state.metrics.queued_bytes += bytes;
  state.metrics.high_water_bytes = (std::max)(state.metrics.high_water_bytes, occupied + bytes);
  state.queue.splice(state.queue.end(), staged); state.waiting_for_keyframe = waiting;
  changed |= state.warning_locked(now);
  if (wake) state.condition.notify_one();
  return finish(Result<void>::success());
}
Result<void> ContinuousRecording::check_health() {
  auto& state = *implementation_;
  std::unique_lock lock(state.mutex);
  const bool changed = state.health_locked(state.clock.now());
  auto result = state.failure ? Result<void>::failure(*state.failure) : Result<void>::success();
  lock.unlock(); if (changed) state.notify_state_changed(); return result;
}
Result<void> ContinuousRecording::stop() {
  auto& state = *implementation_;
  std::scoped_lock lifecycle(state.lifecycle);
  { std::scoped_lock lock(state.mutex); state.accepting = false; state.stopping = true; }
  state.condition.notify_all();
  if (state.writer.joinable()) state.writer.join();
  std::scoped_lock lock(state.mutex);
  return state.failure ? Result<void>::failure(*state.failure) : Result<void>::success();
}
ContinuousQueueMetrics ContinuousRecording::queue_metrics() const {
  auto& state = *implementation_;
  std::scoped_lock lock(state.mutex);
  auto metrics = state.metrics;
  metrics.oldest_enqueue_qpc = state.oldest_locked(); metrics.oldest_age_ticks = state.age_locked(state.clock.now());
  return metrics;
}
bool ContinuousRecording::active() const { auto& s = *implementation_; std::scoped_lock lock(s.mutex); return s.accepting && !s.failure; }
std::uint64_t ContinuousRecording::packets_written() const noexcept { return implementation_->written.load(); }
std::uint64_t ContinuousRecording::failures() const noexcept { return implementation_->failures.load(); }
std::uint64_t ContinuousRecording::packets_accepted() const noexcept { return implementation_->accepted.load(); }
std::string ContinuousRecording::recovery_path() const { auto& s = *implementation_; std::scoped_lock lock(s.mutex); return s.recovery_path; }
}  // namespace rebelliocap
