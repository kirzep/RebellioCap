#pragma once
#include <functional>
#include "audio/audio_capture_source.h"

namespace rebelliocap {
class RecoveringAudioSource final : public IAudioCaptureSource {
 public:
  using SourceResult = Result<std::unique_ptr<IAudioCaptureSource>>;
  RecoveringAudioSource(std::unique_ptr<IAudioCaptureSource> source,
      std::function<SourceResult()> recreate, StreamKind stream, QpcTicks frequency,
      std::function<QpcTicks()> now,
      std::function<void(std::chrono::milliseconds)> wait)
      : source_(std::move(source)), recreate_(std::move(recreate)), stream_(stream),
        frequency_(frequency), now_(std::move(now)), wait_(std::move(wait)), silence_at_(now_()) {}
  bool recovering() const noexcept { return suspended_; }
  Result<std::optional<PcmBlock>> next_block(std::chrono::milliseconds timeout) override {
    using R = Result<std::optional<PcmBlock>>;
    if (!source_ && now_() >= retry_at_) {
      auto candidate = recreate_();
      if (candidate.is_success()) {
        source_ = std::move(candidate).value();
        if (!source_) return R::failure({"audio.recovery_invalid", "Empty audio recovery source", {}});
      } else {
        if (!transient(candidate.error())) return R::failure(candidate.error());
        retry_at_ = now_() + frequency_;
      }
    }
    if (source_) {
      auto result = source_->next_block(timeout);
      if (result.is_success()) {
        if (result.value()) {
          auto& block = *result.value();
          block.discontinuity |= suspended_ || inserted_silence_;
          if (block.sample_rate && block.channels) {
            silence_at_ = (std::max)(silence_at_, block.pts + static_cast<QpcTicks>(
              static_cast<long double>(block.interleaved.size() / block.channels) * frequency_ / block.sample_rate));
          }
          suspended_ = false;
          inserted_silence_ = false;
          return result;
        }
      } else {
        if (!transient(result.error())) return result;
        source_.reset(); // Release COM resources on the same capture worker.
        suspended_ = true;
        retry_at_ = now_() + frequency_;
      }
    }
    // A missing endpoint must not turn the worker into a busy polling loop.
    if (!source_) wait_(timeout);
    const auto now = now_();
    // Loopback may deliver no packets while Windows plays nothing. Permit
    // normal delivery latency, then maintain the source timeline with silence.
    if (!suspended_ && !inserted_silence_ && now - silence_at_ < frequency_ / 4)
      return R::success(std::nullopt);
    if (now - silence_at_ < frequency_ / 100) return R::success(std::nullopt);
    // Keep a maximum 500 ms catch-up after a scheduler stall.
    if (now - silence_at_ > frequency_ / 2) silence_at_ = now - frequency_ / 2;
    PcmBlock silence{stream_, silence_at_, 48'000, 2, std::vector<float>(960, 0.F), 3};
    silence.silent = true;
    silence.discontinuity = true;
    silence_at_ += frequency_ / 100;
    inserted_silence_ = true;
    return R::success(std::move(silence));
  }
 private:
  static bool transient(const Error& error) {
    return error.code == "audio.endpoint_lost" || error.code == "audio.endpoint_unavailable" ||
           error.code == "audio.timestamp_unreliable";
  }
  std::unique_ptr<IAudioCaptureSource> source_;
  std::function<SourceResult()> recreate_;
  StreamKind stream_;
  QpcTicks frequency_;
  std::function<QpcTicks()> now_;
  std::function<void(std::chrono::milliseconds)> wait_;
  QpcTicks retry_at_{0}, silence_at_{0};
  bool suspended_{false}, inserted_silence_{false};
};
}
