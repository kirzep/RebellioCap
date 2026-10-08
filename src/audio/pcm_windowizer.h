#pragma once

#include <optional>
#include <vector>
#include "audio/audio_capture_source.h"
#include "core/qpc_clock.h"

namespace rebelliocap {
struct SynchronizedPcmWindowPair { PcmBlock system; PcmBlock microphone; };

// Mixing copies live on one QPC-anchored 48 kHz sample grid. Source encoders
// receive the original normalized blocks before these copies are queued.
class SynchronizedPcmWindows {
 public:
  explicit SynchronizedPcmWindows(QpcTicks frequency);
  void push_system(const PcmBlock& block);
  void push_microphone(const PcmBlock& block);
  // Wait at most 250 ms of active-source PCM; draining pads the final window.
  std::optional<SynchronizedPcmWindowPair> pop(bool draining = false);
  [[nodiscard]] std::size_t buffered_samples() const noexcept;
  [[nodiscard]] std::uint64_t dropped_frames() const noexcept;
 private:
  class Windowizer {
   public:
    Windowizer();
    void push(const PcmBlock& block, std::int64_t frame, std::optional<std::int64_t> consumed);
    PcmBlock pop(StreamKind stream, std::int64_t frame, QpcTicks pts);
    [[nodiscard]] std::size_t buffered_samples() const noexcept;
    [[nodiscard]] std::int64_t first_frame() const noexcept { return first_frame_; }
    [[nodiscard]] std::int64_t end_frame() const noexcept;
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept { return dropped_frames_; }
   private:
    void discard_before(std::int64_t frame);
    std::int64_t first_frame_{0};
    std::vector<float> samples_;
    std::size_t offset_{0};
    bool discontinuity_{false};
    std::uint64_t dropped_frames_{0};
  };
  void push(Windowizer& queue, const PcmBlock& block);
  QpcTicks frequency_;
  std::optional<QpcTicks> origin_;
  std::int64_t next_frame_{0};
  bool started_{false};
  Windowizer system_;
  Windowizer microphone_;
};
}  // namespace rebelliocap
