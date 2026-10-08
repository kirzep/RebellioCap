#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/qpc_clock.h"

namespace rebelliocap {

enum class StreamKind { Video, MixedAudio, SystemAudio, MicrophoneAudio };

struct EncodedPacket {
  StreamKind stream;
  std::uint32_t epoch;
  QpcTicks pts;
  QpcTicks dts;
  QpcTicks duration;
  bool keyframe;
  std::shared_ptr<const std::vector<std::byte>> payload;
};

}  // namespace rebelliocap
