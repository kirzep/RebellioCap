#pragma once
#include <functional>
#include <string>
#include "capture/capture_source.h"

namespace rebelliocap {
// Only expected desktop transitions are recoverable. Device/encoder faults
// still propagate to the engine and its normal error diagnostics.
inline bool is_transient_capture_error(const Error& error) {
  return error.code == "dxgi_capture.access_lost" ||
         error.code == "dxgi_capture.desktop_access_denied" ||
         error.code == "dxgi_capture.session_disconnected" ||
         error.code == "dxgi_capture.output_not_attached" ||
         error.code == "capture.monitor_unavailable";
}

class RecoveringCaptureSource final : public ICaptureSource {
 public:
  using SourceResult = Result<std::unique_ptr<ICaptureSource>>;
  using Clock = std::chrono::steady_clock;
  using Notify = std::function<void(const std::string&, const Error&)>;
  RecoveringCaptureSource(std::unique_ptr<ICaptureSource> source,
      std::function<SourceResult()> recreate, Notify notify = {},
      std::function<Clock::time_point()> now = [] {return Clock::now();})
      : source_(std::move(source)), recreate_(std::move(recreate)),
        notify_(std::move(notify)), now_(std::move(now)) {}

  bool recovering() const noexcept { return !source_; }
  Result<std::optional<CapturedVideoFrame>> next_frame(std::chrono::milliseconds timeout) override {
    if (!source_) {
      if (now_() < retry_at_) return empty();
      ++attempts_;
      auto recreated = recreate_();
      if (!recreated.is_success()) {
        if (!is_transient_capture_error(recreated.error())) return FrameResult::failure(recreated.error());
        if (attempts_ == 1 || attempts_ % 30 == 0) notify("capture.retry", recreated.error());
        retry_at_ = now_() + std::chrono::seconds(1);
        return empty();
      }
      source_ = std::move(recreated).value();
      // Restoration is confirmed below only after a fresh frame arrives.
    }
    auto frame = source_->next_frame(timeout);
    if (!frame.is_success() && is_transient_capture_error(frame.error())) {
      if (!suspended_) {
        suspended_ = true;
        attempts_ = 0;
        suspended_at_ = now_();
        notify("capture.suspended", frame.error());
      } else if (attempts_ == 1 || attempts_ % 30 == 0) {
        notify("capture.retry", frame.error());
      }
      source_.reset();
      retry_at_ = now_() + std::chrono::seconds(1);
      return empty();
    }
    if (frame.is_success() && frame.value().has_value() && suspended_) {
      suspended_ = false;
      notify("capture.restored", {"capture.restored", "Fresh desktop frame acquired.", {}});
    }
    return frame;
  }
  bool suspended() const noexcept { return suspended_; }
 private:
  using FrameResult = Result<std::optional<CapturedVideoFrame>>;
  static FrameResult empty() { return FrameResult::success(std::nullopt); }
  void notify(const std::string& event, const Error& error) {
    if (!notify_) return;
    auto details = error;
    details.message += " attempts=" + std::to_string(attempts_) + " unavailableMs=" +
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now_() - suspended_at_).count());
    notify_(event, details);
  }
  std::unique_ptr<ICaptureSource> source_;
  std::function<SourceResult()> recreate_;
  Notify notify_;
  std::function<Clock::time_point()> now_;
  Clock::time_point retry_at_{};
  Clock::time_point suspended_at_{};
  std::uint64_t attempts_{0};
  bool suspended_{false};
};
}
