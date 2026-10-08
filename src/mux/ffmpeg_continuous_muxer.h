#pragma once

#include <memory>

#include "mux/continuous_muxer.h"

namespace rebelliocap {

class FfmpegContinuousMuxer final : public IContinuousMuxer {
 public:
  FfmpegContinuousMuxer();
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
