#pragma once
#include <filesystem>
#include "media/stream_descriptor.h"
#include "replay/replay_ring.h"
namespace rebelliocap {
enum class Container { Mp4, Mkv };
class IClipMuxer {
 public:
  virtual ~IClipMuxer() = default;
  virtual Result<std::filesystem::path> write(const ReplaySnapshot&,
      const std::vector<StreamDescriptor>&, const std::filesystem::path&, Container) = 0;
};
}
