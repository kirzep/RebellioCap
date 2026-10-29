#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include "capture/capture_source.h"
#include "capture/d3d11_device.h"
namespace rebelliocap {
// Single capture-thread use. No callbacks touch this object or the immediate
// context. Returned frames own cropped textures on the caller's D3D11 device.
class WgcCaptureSource final : public ICaptureSource {
 public:
  static Result<std::unique_ptr<WgcCaptureSource>> create(const D3d11Device& device,
      std::filesystem::path executable, QpcClock& clock);
  ~WgcCaptureSource() override;
  Result<std::optional<CapturedVideoFrame>> next_frame(std::chrono::milliseconds timeout) override;
  [[nodiscard]] bool waiting_for_game() const noexcept;
 private:
  struct Impl;
  explicit WgcCaptureSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace rebelliocap
