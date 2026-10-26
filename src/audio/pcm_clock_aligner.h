#pragma once
#include <memory>
#include "audio/audio_capture_source.h"
namespace rebelliocap {
struct PcmClockAlignmentStatistics {
  std::uint64_t input_frames=0,output_frames=0,segments=0,compensations=0;
  double maximum_phase_error_frames=0;
};
// One instance per source on the audio worker. Input is normalized48k stereo;
// mixing_pts carries capture QPC, output pts/mixing_pts share a corrected grid.
class PcmClockAligner {
 public:
  explicit PcmClockAligner(QpcTicks frequency);
  ~PcmClockAligner();
  Result<std::vector<PcmBlock>> push(PcmBlock block);
  Result<std::vector<PcmBlock>> flush();
  const PcmClockAlignmentStatistics& statistics() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
