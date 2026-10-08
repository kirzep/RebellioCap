#pragma once

#include <vector>

#include "core/qpc_clock.h"
#include "core/result.h"
#include "media/encoded_packet.h"
#include "video/frame_converter.h"

namespace rebelliocap {

class IVideoEncoder {
 public:
  virtual ~IVideoEncoder() = default;

  // Ownership is intentional: the move-only frame carries the Task 6 pool
  // lease and remains owned by the encoder until asynchronous NVENC work has
  // completed and the registered input has been unmapped.
  virtual Result<std::vector<EncodedPacket>> encode(
      ConvertedVideoFrame frame, QpcTicks pts, bool force_keyframe) = 0;
  virtual Result<std::vector<EncodedPacket>> flush() = 0;
};

}  // namespace rebelliocap
