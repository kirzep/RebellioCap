#pragma once

#include <cstdint>

#include "core/qpc_clock.h"

namespace rebelliocap {

// A presentation clock on the same QPC epoch as audio. Calculate each boundary
// from the origin, rather than accumulating the rounded duration of one frame.
// Callers supply the validated recording FPS and the platform QPC frequency.
class VideoFrameGrid {
 public:
  VideoFrameGrid(QpcTicks origin, std::uint32_t fps, QpcTicks frequency)
      : origin_(origin), fps_(fps), frequency_(frequency) {}

  [[nodiscard]] QpcTicks at(std::uint64_t index) const noexcept {
    return origin_ + static_cast<QpcTicks>(index / fps_) * frequency_ +
           static_cast<QpcTicks>(index % fps_) * frequency_ / fps_;
  }

  [[nodiscard]] std::uint64_t index_at(QpcTicks time) const noexcept {
    if (time <= origin_) return 0;
    const auto elapsed = time - origin_;
    auto index = static_cast<std::uint64_t>(elapsed / frequency_) * fps_ +
                 static_cast<std::uint64_t>((elapsed % frequency_) * fps_ / frequency_);
    // at() rounds boundaries down to whole QPC ticks. Invert that rounding at
    // the boundary itself (e.g. 166666 ticks is already frame 1 at 60 FPS).
    if (at(index + 1) <= time) ++index;
    return index;
  }

  [[nodiscard]] QpcTicks at_or_before(QpcTicks time) const noexcept {
    return at(index_at(time));
  }

 private:
  QpcTicks origin_;
  std::uint32_t fps_;
  QpcTicks frequency_;
};

}  // namespace rebelliocap
