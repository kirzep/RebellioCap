#pragma once
#include <chrono>
#include <optional>
#include "core/result.h"
#include "media/encoded_packet.h"

namespace rebelliocap {
struct PcmBlock {
  StreamKind stream;
  QpcTicks pts;
  std::uint32_t sample_rate;
  std::uint16_t channels;
  std::vector<float> interleaved;
  std::uint32_t channel_mask = 0;
  bool silent = false;
  bool discontinuity = false;
  bool timestamp_reconstructed = false;
  QpcTicks raw_pts = 0;
  std::uint64_t device_position = 0;
  bool device_position_valid = false;
  // Trusted capture QPC for the first normalized sample, used only by mixing.
  // pts remains the contiguous source-encoder timeline; raw_pts is provenance.
  std::optional<QpcTicks> mixing_pts;
};
class IAudioCaptureSource {
 public:
  virtual ~IAudioCaptureSource() = default;
  virtual Result<std::optional<PcmBlock>> next_block(std::chrono::milliseconds timeout) = 0;
};
}
