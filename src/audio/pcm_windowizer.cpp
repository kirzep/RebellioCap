#include "audio/pcm_windowizer.h"
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rebelliocap {
namespace {
constexpr std::int64_t kWindowFrames = 480;
constexpr std::int64_t kMaximumWaitFrames = 12'000;
constexpr std::int64_t kMaximumFrames = 24'000;
constexpr std::size_t kCompactionThreshold = 9'600;
}

SynchronizedPcmWindows::Windowizer::Windowizer() { samples_.reserve(kMaximumFrames * 2); }
std::size_t SynchronizedPcmWindows::Windowizer::buffered_samples() const noexcept {
  return samples_.size() - offset_;
}
std::int64_t SynchronizedPcmWindows::Windowizer::end_frame() const noexcept {
  return first_frame_ + static_cast<std::int64_t>(buffered_samples() / 2);
}
void SynchronizedPcmWindows::Windowizer::discard_before(std::int64_t frame) {
  const auto count = (std::clamp)(frame - first_frame_, std::int64_t{0},
                                static_cast<std::int64_t>(buffered_samples() / 2));
  offset_ += static_cast<std::size_t>(count) * 2;
  first_frame_ += count;
  if (offset_ >= kCompactionThreshold || offset_ == samples_.size()) {
    samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(offset_));
    offset_ = 0;
  }
}
void SynchronizedPcmWindows::Windowizer::push(
    const PcmBlock& block, std::int64_t frame, std::optional<std::int64_t> consumed) {
  const auto frames = static_cast<std::int64_t>(block.interleaved.size() / 2);
  if (!frames) return;
  const auto input_end = frame + frames;
  // Already emitted windows (including synthetic silence) are immutable.
  auto begin = (std::max)(frame, consumed.value_or(frame));
  if (buffered_samples()) begin = (std::max)(begin, end_frame());
  if (begin >= input_end) return;
  // Retain only the final 500 ms, including gaps. Never allocate a huge
  // silence buffer for a device timestamp jump or oversized capture batch.
  const auto retain_from = input_end - kMaximumFrames;
  if (buffered_samples() && retain_from > first_frame_) {
    const auto removed = (std::min)(retain_from, end_frame()) - first_frame_;
    dropped_frames_ += static_cast<std::uint64_t>(removed);
    discard_before(retain_from);
    discontinuity_ = true;
  }
  if (begin < retain_from) {
    dropped_frames_ += static_cast<std::uint64_t>(retain_from - begin);
    begin = retain_from;
    discontinuity_ = true;
  }
  if (!buffered_samples()) first_frame_ = begin;
  const auto gap = begin - end_frame();
  const auto count = static_cast<std::size_t>(input_end - begin) * 2;
  if (offset_ && samples_.size() + static_cast<std::size_t>(gap) * 2 + count > kMaximumFrames * 2) {
    samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(offset_));
    offset_ = 0;
  }
  if (gap) {
    samples_.insert(samples_.end(), static_cast<std::size_t>(gap) * 2, 0.F);
  }
  // Silence-filled gaps and trimmed overlaps produce contiguous mixing PCM.
  // Preserve actual source discontinuity/overflow for the epoch policy.
  discontinuity_ |= block.discontinuity;
  const auto input = block.interleaved.begin() + static_cast<std::ptrdiff_t>((begin - frame) * 2);
  if (block.silent) samples_.insert(samples_.end(), count, 0.F);
  else samples_.insert(samples_.end(), input, block.interleaved.end());
}
PcmBlock SynchronizedPcmWindows::Windowizer::pop(
    StreamKind stream, std::int64_t frame, QpcTicks pts) {
  discard_before(frame);
  PcmBlock result{stream, pts, 48'000, 2, std::vector<float>(kWindowFrames * 2, 0.F), 3};
  const auto begin = (std::max)(frame, first_frame_);
  const auto end = (std::min)(frame + kWindowFrames, end_frame());
  if (begin < end) {
    const auto source = offset_ + static_cast<std::size_t>(begin - first_frame_) * 2;
    std::copy_n(samples_.begin() + static_cast<std::ptrdiff_t>(source),
                static_cast<std::size_t>(end - begin) * 2,
                result.interleaved.begin() + static_cast<std::ptrdiff_t>((begin - frame) * 2));
  }
  result.silent = std::all_of(result.interleaved.begin(), result.interleaved.end(),
                            [](float sample) { return sample == 0.F; });
  result.discontinuity = discontinuity_;
  discontinuity_ = false;
  discard_before(frame + kWindowFrames);
  return result;
}
SynchronizedPcmWindows::SynchronizedPcmWindows(QpcTicks frequency) : frequency_(frequency) {}
void SynchronizedPcmWindows::push(Windowizer& queue, const PcmBlock& block) {
  const auto capture_pts = block.mixing_pts.value_or(block.pts);
  if (!origin_) origin_ = capture_pts;
  // Quantize absolute timestamps once; accumulating rounded window durations
  // must not move impulses or lose fractional QPC ticks.
  const auto frame = static_cast<std::int64_t>(std::llround(
      (static_cast<long double>(capture_pts) - *origin_) * 48'000 / frequency_));
  queue.push(block, frame, started_ ? std::optional{next_frame_} : std::nullopt);
}
void SynchronizedPcmWindows::push_system(const PcmBlock& block) { push(system_, block); }
void SynchronizedPcmWindows::push_microphone(const PcmBlock& block) { push(microphone_, block); }
std::optional<SynchronizedPcmWindowPair> SynchronizedPcmWindows::pop(bool draining) {
  if (!buffered_samples()) return std::nullopt;
  const auto first = !system_.buffered_samples() ? microphone_.first_frame()
      : !microphone_.buffered_samples() ? system_.first_frame()
      : (std::min)(system_.first_frame(), microphone_.first_frame());
  if (!started_) next_frame_ = first;
  // Skip only a lost interval after overflow, retaining small/startup gaps.
  else if (first - next_frame_ >= kMaximumFrames) next_frame_ = first;
  const auto ready = [this](const Windowizer& queue) {
    return queue.buffered_samples() && queue.end_frame() >= next_frame_ + kWindowFrames;
  };
  const auto latest = (std::max)(system_.buffered_samples() ? system_.end_frame() : next_frame_,
                               microphone_.buffered_samples() ? microphone_.end_frame() : next_frame_);
  if (!draining && !(ready(system_) && ready(microphone_)) &&
      latest - next_frame_ < kMaximumWaitFrames) return std::nullopt;
  const auto pts = *origin_ + static_cast<QpcTicks>(
      static_cast<long double>(next_frame_) * frequency_ / 48'000);
  auto pair = SynchronizedPcmWindowPair{system_.pop(StreamKind::SystemAudio, next_frame_, pts),
                                      microphone_.pop(StreamKind::MicrophoneAudio, next_frame_, pts)};
  next_frame_ += kWindowFrames;
  started_ = true;
  return pair;
}
std::size_t SynchronizedPcmWindows::buffered_samples() const noexcept {
  return system_.buffered_samples() + microphone_.buffered_samples();
}
std::uint64_t SynchronizedPcmWindows::dropped_frames() const noexcept {
  return system_.dropped_frames() + microphone_.dropped_frames();
}
}  // namespace rebelliocap
