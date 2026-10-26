#pragma once

#include <optional>
#include <deque>
#include "core/qpc_clock.h"

namespace rebelliocap::detail {
// Decode order trails presentation order by the configured B-frame depth.
class NvencDecodeTimeline {
 public:
  explicit NvencDecodeTimeline(std::uint32_t depth = 2) : depth_(depth) {}
  void submit(QpcTicks pts, QpcTicks duration) {
    if (!initialized_) {
      for (auto offset = depth_; offset > 0; --offset)
        timestamps_.push_back(pts - static_cast<QpcTicks>(offset) * duration);
      initialized_ = true;
    }
    // Shift submitted timestamps by two frames, not two nominal durations.
    // Missed capture deadlines must leave the same holes on both clocks.
    timestamps_.push_back(pts);
  }
  std::optional<QpcTicks> take() {
    if (timestamps_.empty()) return std::nullopt;
    const auto result = timestamps_.front();
    timestamps_.pop_front();
    return result;
  }
 private:
  std::deque<QpcTicks> timestamps_;
  bool initialized_{};
  std::uint32_t depth_;
};
}
