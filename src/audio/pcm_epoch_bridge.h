#pragma once
#include <functional>
#include <algorithm>
#include <cmath>
#include "audio/audio_capture_source.h"
namespace rebelliocap {
// Each source keeps one AAC encoder. Recovery gaps become explicit silence;
// samples that overlap already emitted silence are never played twice.
class PcmEpochBridge {
 public:
  explicit PcmEpochBridge(QpcTicks frequency) : frequency_(frequency) {}
  using Sink = std::function<Result<void>(const PcmBlock&)>;
  Result<void> accept(PcmBlock block, const Sink& sink) {
    if (frequency_ <= 0 || block.sample_rate != 48'000 || block.channels != 2 ||
        block.interleaved.empty() || block.interleaved.size() % 2 || block.pts < 0)
      return Result<void>::failure({"audio.epoch_invalid", "Expected normalized stereo PCM", {}});
    if (!anchor_) anchor_ = block.pts;
    aligning_ |= block.discontinuity;
    if (aligning_ && frames_) {
      const auto capture = block.mixing_pts.value_or(block.pts);
      const auto target = static_cast<std::int64_t>(std::llround(
          (static_cast<long double>(capture) - *anchor_) * 48'000 / frequency_));
      auto gap = target - frames_;
      // Bound catch-up work and packet allocation after a suspended process.
      if (gap > 96'000) return Result<void>::failure({"audio.recovery_gap_too_large",
        "Capture worker paused for more than two seconds; restart recording", {}});
      while (gap > 0) {
        const auto count = (std::min)(gap, std::int64_t{480});
        PcmBlock silence{block.stream, pts(), 48'000, 2,
          std::vector<float>(static_cast<std::size_t>(count) * 2, 0.F), 3};
        silence.silent = true;
        silence.mixing_pts = silence.pts;
        auto result = sink(silence);
        if (!result.is_success()) return result;
        frames_ += count; gap -= count;
      }
      if (gap < 0) {
        const auto trim = (std::min)(-gap, static_cast<std::int64_t>(block.interleaved.size() / 2));
        block.interleaved.erase(block.interleaved.begin(), block.interleaved.begin() + trim * 2);
        if (block.interleaved.empty()) return Result<void>::success();
      }
      block.mixing_pts = pts();
    }
    block.pts = pts();
    block.discontinuity = false;
    auto result = sink(block);
    if (result.is_success()) {
      frames_ += static_cast<std::int64_t>(block.interleaved.size() / 2);
      aligning_ = false;
    }
    return result;
  }
 private:
  QpcTicks pts() const { return *anchor_ + static_cast<QpcTicks>(
    static_cast<long double>(frames_) * frequency_ / 48'000); }
  QpcTicks frequency_;
  std::optional<QpcTicks> anchor_;
  std::int64_t frames_{0};
  bool aligning_{false};
};
}
