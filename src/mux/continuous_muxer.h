#pragma once

#include <filesystem>
#include <vector>
#include <optional>

#include "core/result.h"
#include "media/encoded_packet.h"
#include "media/stream_descriptor.h"
#include "mux/clip_muxer.h"

namespace rebelliocap {

class IContinuousMuxer {
 public:
  virtual ~IContinuousMuxer() = default;
  virtual Result<void> open(const std::vector<StreamDescriptor>& descriptors,
                            const std::filesystem::path& destination,
                            Container container) = 0;
  virtual Result<void> write(const EncodedPacket& packet) = 0;
  virtual Result<std::filesystem::path> finalize() = 0;
  virtual void abort() noexcept = 0;
  // Available after abort/finalization failure; empty means nothing retained.
  virtual std::optional<std::filesystem::path> recovery_path() const { return {}; }
};

}  // namespace rebelliocap
