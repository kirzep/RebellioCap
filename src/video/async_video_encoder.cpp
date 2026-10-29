#include "video/async_video_encoder.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>

namespace rebelliocap {
namespace {
constexpr auto kAdmissionRetry = std::chrono::milliseconds(2);

class NvencWorkerBackend final : public IAsyncVideoEncoderBackend {
 public:
  explicit NvencWorkerBackend(std::unique_ptr<NvencEncoder> encoder)
      : encoder_(std::move(encoder)) {}
  Result<EncoderAdmission> prepare_frame(bool force_keyframe) override {
    return encoder_->prepare_frame(force_keyframe);
  }
  Result<std::vector<EncodedPacket>> encode(
      ConvertedVideoFrame frame, QpcTicks pts, bool force_keyframe) override {
    return encoder_->encode(std::move(frame), pts, force_keyframe);
  }
  Result<std::vector<EncodedPacket>> flush() override { return encoder_->flush(); }
  const StreamDescriptor& descriptor() const noexcept override { return encoder_->descriptor(); }
  NvencRuntimeMetrics metrics() const override { return encoder_->metrics(); }
  std::shared_ptr<const NvencLifetimeMetrics> lifetime_metrics() const noexcept override {
    return encoder_->lifetime_metrics();
  }
 private:
  std::unique_ptr<NvencEncoder> encoder_;
};

Error stopped_error() {
  return {"async_encoder.stopped", "The encoder worker has already stopped accepting frames.", {}};
}
}  // namespace

struct AsyncVideoEncoder::Impl {
  struct QueuedFrame {
    ConvertedVideoFrame frame;
    QpcTicks pts;
    bool keyframe;
  };
  explicit Impl(std::size_t capacity) {
    queue_stats.queue_capacity = static_cast<std::uint64_t>(capacity);
  }

  mutable std::mutex mutex;
  std::mutex join_mutex;
  std::condition_variable changed;
  std::thread worker;
  std::deque<QueuedFrame> queue;
  std::vector<EncodedPacket> completed;
  std::optional<Error> failure;
  StreamDescriptor stream{};
  NvencRuntimeMetrics runtime;
  AsyncVideoQueueMetrics queue_stats;
  std::shared_ptr<const NvencLifetimeMetrics> lifetime;
  bool ready{false}, closing{false}, pending_keyframe{false};

  void fail(Error error) {
    std::lock_guard lock(mutex);
    if (!failure) failure = std::move(error);
    closing = true;
    changed.notify_all();
  }
  void publish(std::vector<EncodedPacket> packets, NvencRuntimeMetrics metrics) {
    std::lock_guard lock(mutex);
    completed.insert(completed.end(), std::make_move_iterator(packets.begin()),
                     std::make_move_iterator(packets.end()));
    runtime = std::move(metrics);
    // Backend admission retries are pressure observations, not dropped frames.
    runtime.backpressure_dropped_frames = queue_stats.dropped_frames;
  }
  void run(std::unique_ptr<IAsyncVideoEncoderBackend> backend) noexcept {
    std::optional<QueuedFrame> current;
    try {
      // Snapshot initialization is the only startup rendezvous. No NVENC work
      // runs under the producer mutex, including these backend accessors.
      auto descriptor = backend->descriptor();
      auto initial_metrics = backend->metrics();
      auto lifetime_metrics = backend->lifetime_metrics();
      {
        std::lock_guard lock(mutex);
        stream = std::move(descriptor);
        runtime = std::move(initial_metrics);
        lifetime = std::move(lifetime_metrics);
        ready = true;
      }
      changed.notify_all();
      for (;;) {
        {
          std::unique_lock lock(mutex);
          if (!current) {
            if (queue.empty() && !closing && runtime.in_flight_frames == 0) {
              changed.wait(lock, [&] { return closing || !queue.empty(); });
            }
            if (failure || (closing && queue.empty())) break;
            if (!queue.empty()) {
              current.emplace(std::move(queue.front()));
              queue.pop_front();
            }
          }
        }
        auto admission = backend->prepare_frame(current && current->keyframe);
        if (!admission.is_success()) { fail(admission.error()); break; }
        const bool can_submit = admission.value().can_submit;
        publish(std::move(admission.value().completed), backend->metrics());
        if (current && can_submit) {
          auto encoded = backend->encode(std::move(current->frame), current->pts, current->keyframe);
          current.reset();
          {
            std::lock_guard lock(mutex);
            --queue_stats.queue_depth;
          }
          if (!encoded.is_success()) { fail(encoded.error()); break; }
          publish(std::move(encoded).value(), backend->metrics());
        } else {
          // A finite sleeping retry keeps completions/watchdog moving without
          // polling on the capture thread or spinning while NVENC is full.
          std::unique_lock lock(mutex);
          changed.wait_for(lock, kAdmissionRetry);
        }
      }
      auto flushed = backend->flush();
      if (!flushed.is_success()) fail(flushed.error());
      else publish(std::move(flushed).value(), backend->metrics());
    } catch (const std::exception& exception) {
      fail({"async_encoder.worker_exception", exception.what(), {}});
    } catch (...) {
      fail({"async_encoder.worker_exception", "The encoder worker raised an unknown exception.", {}});
    }
    current.reset();
    {
      std::lock_guard lock(mutex);
      queue.clear();
      queue_stats.queue_depth = 0;
      ready = true;
      closing = true;
    }
    // NvencEncoder shutdown retains its own watchdog and unsafe-device policy.
    // Destruction stays on the same worker as resource registration/submission.
    backend.reset();
    changed.notify_all();
  }
};

AsyncVideoEncoder::AsyncVideoEncoder(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

AsyncVideoEncoder::~AsyncVideoEncoder() { static_cast<void>(flush()); }

Result<std::unique_ptr<AsyncVideoEncoder>> AsyncVideoEncoder::create(
    std::unique_ptr<NvencEncoder> encoder, std::size_t capacity) {
  if (!encoder) return Result<std::unique_ptr<AsyncVideoEncoder>>::failure(
      {"async_encoder.invalid_backend", "An NVENC encoder is required.", {}});
  return create(std::make_unique<NvencWorkerBackend>(std::move(encoder)), capacity);
}

Result<std::unique_ptr<AsyncVideoEncoder>> AsyncVideoEncoder::create(
    std::unique_ptr<IAsyncVideoEncoderBackend> backend, std::size_t capacity) {
  if (!backend || capacity == 0 || capacity > 2) {
    return Result<std::unique_ptr<AsyncVideoEncoder>>::failure(
        {"async_encoder.invalid_configuration", "An encoder backend and queue capacity of one or two are required.", {}});
  }
  auto result = std::unique_ptr<AsyncVideoEncoder>(new AsyncVideoEncoder(std::make_unique<Impl>(capacity)));
  auto* impl = result->implementation_.get();
  try {
    impl->worker = std::thread([impl, backend = std::move(backend)]() mutable { impl->run(std::move(backend)); });
  } catch (const std::exception& exception) {
    return Result<std::unique_ptr<AsyncVideoEncoder>>::failure(
        {"async_encoder.worker_start_failed", exception.what(), {}});
  }
  std::unique_lock lock(impl->mutex);
  impl->changed.wait(lock, [&] { return impl->ready; });
  if (impl->failure) return Result<std::unique_ptr<AsyncVideoEncoder>>::failure(*impl->failure);
  return Result<std::unique_ptr<AsyncVideoEncoder>>::success(std::move(result));
}

Result<EncoderAdmission> AsyncVideoEncoder::prepare_frame(bool force_keyframe) {
  auto& impl = *implementation_;
  std::lock_guard lock(impl.mutex);
  if (impl.failure) return Result<EncoderAdmission>::failure(*impl.failure);
  if (impl.closing) return Result<EncoderAdmission>::failure(stopped_error());
  impl.pending_keyframe |= force_keyframe;
  const bool capacity = impl.queue_stats.queue_depth < impl.queue_stats.queue_capacity;
  if (!capacity) ++impl.queue_stats.dropped_frames;
  return Result<EncoderAdmission>::success({std::exchange(impl.completed, {}), capacity});
}

Result<std::vector<EncodedPacket>> AsyncVideoEncoder::encode(
    ConvertedVideoFrame frame, QpcTicks pts, bool force_keyframe) {
  auto& impl = *implementation_;
  std::lock_guard lock(impl.mutex);
  if (impl.failure) return Result<std::vector<EncodedPacket>>::failure(*impl.failure);
  if (impl.closing) return Result<std::vector<EncodedPacket>>::failure(stopped_error());
  impl.pending_keyframe |= force_keyframe;
  if (impl.queue_stats.queue_depth >= impl.queue_stats.queue_capacity) {
    ++impl.queue_stats.dropped_frames;
  } else {
    impl.queue.push_back({std::move(frame), pts, impl.pending_keyframe});
    impl.pending_keyframe = false;
    ++impl.queue_stats.queue_depth;
    impl.queue_stats.max_queue_depth = (std::max)(impl.queue_stats.max_queue_depth, impl.queue_stats.queue_depth);
    impl.changed.notify_one();
  }
  return Result<std::vector<EncodedPacket>>::success(std::exchange(impl.completed, {}));
}

Result<std::vector<EncodedPacket>> AsyncVideoEncoder::flush() {
  auto& impl = *implementation_;
  std::lock_guard join_lock(impl.join_mutex);
  {
    std::lock_guard lock(impl.mutex);
    impl.closing = true;
  }
  impl.changed.notify_one();
  if (impl.worker.joinable()) impl.worker.join();
  std::lock_guard lock(impl.mutex);
  if (impl.failure) return Result<std::vector<EncodedPacket>>::failure(*impl.failure);
  return Result<std::vector<EncodedPacket>>::success(std::exchange(impl.completed, {}));
}

const StreamDescriptor& AsyncVideoEncoder::descriptor() const noexcept { return implementation_->stream; }
NvencRuntimeMetrics AsyncVideoEncoder::metrics() const {
  std::lock_guard lock(implementation_->mutex);
  auto snapshot = implementation_->runtime;
  snapshot.backpressure_dropped_frames = implementation_->queue_stats.dropped_frames;
  return snapshot;
}
AsyncVideoQueueMetrics AsyncVideoEncoder::queue_metrics() const {
  std::lock_guard lock(implementation_->mutex);
  return implementation_->queue_stats;
}
std::shared_ptr<const NvencLifetimeMetrics> AsyncVideoEncoder::lifetime_metrics() const noexcept {
  return implementation_->lifetime;
}
}  // namespace rebelliocap
