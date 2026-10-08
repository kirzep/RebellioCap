#pragma once
#include "audio/wasapi_pcm.h"
namespace rebelliocap::audio_detail {
// Normal raw QPC regression allowance is 250us. Intact device samples with a
// residual above one packet period +250us and up to that period +2ms reconstruct
// from the trusted anchor. This bounded jitter repair does not compensate A/V drift.
// Contiguous device frames are mandatory. Resets/gaps need a new source epoch.
class WasapiTimeline {
 public:
  explicit WasapiTimeline(QpcTicks frequency) : frequency_(frequency) {}
  Result<void> stamp(PcmBlock& block, std::uint64_t position, std::uint32_t frames);
 private:
  QpcTicks frequency_;
  bool initialized_ = false;
  bool invalid_ = false;
  QpcTicks anchor_ = 0, previous_ = 0;
  std::uint64_t anchor_position_ = 0, previous_position_ = 0;
  std::uint32_t previous_frames_ = 0, rate_ = 0;
};
}
