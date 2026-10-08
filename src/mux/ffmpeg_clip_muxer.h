#pragma once
#include "mux/clip_muxer.h"
#include <functional>
namespace rebelliocap {
// End is an inclusive requested PTS boundary. The complete decode prefix is
// retained, including at most two future reference frames. Container metadata
// reports the requested bounds separately from actual presentation end.
class FfmpegClipMuxer final : public IClipMuxer {
 public:
 Result<std::filesystem::path> write(const ReplaySnapshot&,
     const std::vector<StreamDescriptor>&, const std::filesystem::path&, Container) override;
 private:
  friend struct FfmpegMuxTestAccess;
  // Private deterministic file-lifecycle observation/failure seam.
  std::function<void(const std::filesystem::path&, bool)> lifecycle_observer_;
};
}
