#pragma once
#include <memory>
#include "audio/audio_capture_source.h"
namespace rebelliocap {
struct PcmNormalizationStatistics {
 std::uint64_t accepted_frames=0, emitted_frames=0, reconstructed_input_blocks=0, segments=0;
 QpcTicks maximum_input_residual_ticks=0;
};
// Use, move and destroy on the creating COM thread. One instance per source. Empty output means buffered input, not silence.
// Output raw_pts is the segment input anchor raw timestamp; reconstruction flag
// means this segment has consumed reconstructed input. Use statistics for exact counts.
// Device positions are not carried across rate conversion. Input residual stats
// remain available for diagnostics; device continuity never trims source samples.
// mixing_pts carries trusted capture QPC independently of contiguous encoder pts.
// Buffered resampling splits output at input timing boundaries (within one output
// sample of rate-conversion quantization), retaining the corresponding QPC anchor.
// flush is end-of-stream (idempotent); subsequent normalize is rejected.
class PcmNormalizer {
 public:
 explicit PcmNormalizer(QpcClock&);
 ~PcmNormalizer();
 PcmNormalizer(PcmNormalizer&&) noexcept;
 PcmNormalizer& operator=(PcmNormalizer&&) noexcept;
 // Passing an owned block avoids PCM copies when it is already 48 kHz stereo.
 Result<std::vector<PcmBlock>> normalize(PcmBlock,StreamKind);
 Result<std::vector<PcmBlock>> flush();
 const PcmNormalizationStatistics& statistics() const noexcept;
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
