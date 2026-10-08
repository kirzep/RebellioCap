#include "audio/wasapi_timeline.h"
#include <limits>
#include <string>
namespace rebelliocap::audio_detail {
namespace {
constexpr QpcTicks kRegressionBoundMicroseconds = 250;
constexpr QpcTicks kRawQpcJitterBoundMicroseconds = 2'000;
Result<QpcTicks> frame_ticks(std::uint64_t frames, QpcTicks frequency, std::uint32_t rate) {
  const auto limit = static_cast<std::uint64_t>(INT64_MAX);
  if (!rate || frequency <= 0) return Result<QpcTicks>::failure({"audio.timestamp_invalid", "Invalid timestamp rate", {}});
  const auto f = static_cast<std::uint64_t>(frequency);
  const auto whole = frames / rate;
  if (whole > limit / f) return Result<QpcTicks>::failure({"audio.timestamp_invalid", "Frame offset overflow", {}});
  const auto rem = frames % rate;
  const auto fraction = rem * (f / rate) + rem * (f % rate) / rate;
  if (fraction > limit - whole * f) return Result<QpcTicks>::failure({"audio.timestamp_invalid", "Frame offset overflow", {}});
  return Result<QpcTicks>::success(static_cast<QpcTicks>(whole * f + fraction));
}
}
Result<void> WasapiTimeline::stamp(PcmBlock& block, std::uint64_t position, std::uint32_t frames) {
  auto fail = [&](std::string message) { invalid_ = true; return Result<void>::failure({"audio.timestamp_invalid", std::move(message), {}}); };
  block.raw_pts = block.pts;
  block.device_position = position;
  block.device_position_valid = true;
  block.timestamp_reconstructed = false;
  if (invalid_ || !frames || position > UINT64_MAX - frames || block.pts < 0 || !block.sample_rate || block.sample_rate > 768000 || frequency_ <= 0) return fail("Invalid audio timeline epoch");
  if (!initialized_) {
    initialized_ = true; anchor_ = block.pts; anchor_position_ = position; rate_ = block.sample_rate;
  } else {
    if (block.discontinuity || block.sample_rate != rate_ || position != previous_position_ + previous_frames_ || position < anchor_position_) {
      invalid_ = true;
      return Result<void>::failure({"audio.epoch_required", "Audio device position gap/reset/discontinuity requires a new epoch", {}});
    }
    auto offset = frame_ticks(position - anchor_position_, frequency_, rate_);
    auto period = frame_ticks(previous_frames_, frequency_, rate_);
    if (!offset.is_success() || !period.is_success() || anchor_ > INT64_MAX - offset.value()) return fail("Audio timestamp prediction overflow");
    const auto predicted = anchor_ + offset.value();
    const auto tolerance = frequency_ / (1000000 / kRegressionBoundMicroseconds);
    const auto jitter_tolerance = frequency_ / (1000000 / kRawQpcJitterBoundMicroseconds);
    if (period.value() > INT64_MAX - jitter_tolerance) return fail("Audio timestamp bound overflow");
    const auto residual = block.pts >= predicted ? block.pts - predicted : predicted - block.pts;
    if (residual > period.value() + tolerance) {
      // Device frames above are intact. Repair a bounded raw QPC anomaly using
      // their sample count and retain the last trusted anchor. The 2ms allowance
      // extends one packet period; it never applies an A/V drift offset.
      if (residual <= period.value() + jitter_tolerance) {
        if (predicted <= previous_) return fail("Audio reconstructed timestamp did not advance");
        block.pts = predicted;
        block.timestamp_reconstructed = true;
      } else {
        invalid_ = true;
        return Result<void>::failure({"audio.timestamp_unreliable",
                    "Audio raw timestamp jumped beyond one buffer period raw=" +
                    std::to_string(block.pts) + " predicted=" + std::to_string(predicted) +
                    " residual=" + std::to_string(residual) + " period=" +
                    std::to_string(period.value()) + " tolerance=" + std::to_string(tolerance) +
                    " recovery_tolerance=" + std::to_string(jitter_tolerance) +
                    " position=" + std::to_string(position) + " previous_position=" +
                    std::to_string(previous_position_), {}});
      }
    } else if (block.pts <= previous_) {
      if (previous_ - block.pts > tolerance) return fail("Audio raw timestamp regression exceeded tolerance");
      if (predicted <= previous_) return fail("Audio reconstructed timestamp did not advance");
      block.pts = predicted;
      block.timestamp_reconstructed = true;
    } else {
      anchor_ = block.pts; anchor_position_ = position;
    }
  }
  previous_ = block.pts; previous_position_ = position; previous_frames_ = frames;
  return Result<void>::success();
}
}
