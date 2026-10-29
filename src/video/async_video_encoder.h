#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "video/nvenc_encoder.h"

namespace rebelliocap {

// The worker owns this backend and invokes every method (including destruction)
// on its thread. The real NVENC session is initialized before ownership transfer.
class IAsyncVideoEncoderBackend : public IVideoEncoder {
 public:
  virtual Result<EncoderAdmission> prepare_frame(bool force_keyframe) = 0;
  [[nodiscard]] virtual const StreamDescriptor& descriptor() const noexcept = 0;
  [[nodiscard]] virtual NvencRuntimeMetrics metrics() const = 0;
  [[nodiscard]] virtual std::shared_ptr<const NvencLifetimeMetrics>
  lifetime_metrics() const noexcept = 0;
};

struct AsyncVideoQueueMetrics {
  // Includes the frame currently waiting for backend admission. Submitted NVENC
  // slots are tracked separately by NvencRuntimeMetrics::in_flight_frames.
  std::uint64_t queue_depth{0};
  std::uint64_t queue_capacity{0};
  std::uint64_t max_queue_depth{0};
  std::uint64_t dropped_frames{0};
};

class AsyncVideoEncoder final : public IVideoEncoder {
 public:
  // At most two additional leases fit beside the six NVENC slots in pool8.
  static Result<std::unique_ptr<AsyncVideoEncoder>> create(
      std::unique_ptr<NvencEncoder> encoder, std::size_t capacity = 2);
  static Result<std::unique_ptr<AsyncVideoEncoder>> create(
      std::unique_ptr<IAsyncVideoEncoderBackend> backend, std::size_t capacity = 2);
  ~AsyncVideoEncoder() override;
  AsyncVideoEncoder(const AsyncVideoEncoder&) = delete;
  AsyncVideoEncoder& operator=(const AsyncVideoEncoder&) = delete;

  // Single capture producer. These methods only take a short queue mutex; no
  // backend calls or waits for driver work occur on the producer thread.
  Result<EncoderAdmission> prepare_frame(bool force_keyframe);
  Result<std::vector<EncodedPacket>> encode(
      ConvertedVideoFrame frame, QpcTicks pts, bool force_keyframe) override;
  // Drains accepted frames, flushes the backend and joins its worker. Shutdown
  // uses the backend's existing completion watchdog, with sleeping retries.
  Result<std::vector<EncodedPacket>> flush() override;
  [[nodiscard]] const StreamDescriptor& descriptor() const noexcept;
  [[nodiscard]] NvencRuntimeMetrics metrics() const;
  [[nodiscard]] AsyncVideoQueueMetrics queue_metrics() const;
  [[nodiscard]] std::shared_ptr<const NvencLifetimeMetrics> lifetime_metrics()
      const noexcept;

 private:
  struct Impl;
  explicit AsyncVideoEncoder(std::unique_ptr<Impl> implementation) noexcept;
  std::unique_ptr<Impl> implementation_;
};

}  // namespace rebelliocap
