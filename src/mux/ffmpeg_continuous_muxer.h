#pragma once

#include <memory>
#include <cstddef>

#include "mux/continuous_muxer.h"

namespace rebelliocap {
class PipelineTelemetry;

class FfmpegContinuousMuxer final : public IContinuousMuxer {
 public:
  static constexpr std::size_t kDefaultAvioBufferBytes = 32 * 1024;
  explicit FfmpegContinuousMuxer(std::size_t avio_buffer_bytes = kDefaultAvioBufferBytes,
                                 std::shared_ptr<PipelineTelemetry> telemetry = {});
  ~FfmpegContinuousMuxer() override;

  FfmpegContinuousMuxer(const FfmpegContinuousMuxer&) = delete;
  FfmpegContinuousMuxer& operator=(const FfmpegContinuousMuxer&) = delete;

  Result<void> open(const std::vector<StreamDescriptor>& descriptors,
                    const std::filesystem::path& destination,
                    Container container) override;
  Result<void> write(const EncodedPacket& packet) override;
  Result<std::filesystem::path> finalize() override;
  void abort() noexcept override;
  std::optional<std::filesystem::path> recovery_path() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> implementation_;
};

}  // namespace rebelliocap
