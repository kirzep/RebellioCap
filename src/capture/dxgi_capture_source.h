#pragma once

#include <Windows.h>
#include <dxgi1_2.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "capture/capture_source.h"
#include "capture/d3d11_device.h"
#include "capture/monitor_catalog.h"

namespace rebelliocap {

enum class CaptureFailureClass {
  RecoverableAccessLost,
  DesktopAccessDenied,
  SessionDisconnected,
  Unsupported,
  DuplicationSessionLimit,
  Fatal
};

class DuplicationFrameLease;

[[nodiscard]] CaptureFailureClass classify_dxgi_capture_failure(HRESULT result) noexcept;
[[nodiscard]] Error dxgi_capture_error(std::string operation, HRESULT result);

class DxgiCaptureSource final : public ICaptureSource {
 public:
  ~DxgiCaptureSource() override;

  static Result<std::unique_ptr<DxgiCaptureSource>> create(
      const D3d11Device& device, MonitorId monitor, QpcClock& clock);

  Result<std::optional<CapturedVideoFrame>> next_frame(
      std::chrono::milliseconds timeout) override;

  [[nodiscard]] DXGI_OUTDUPL_DESC description() const noexcept;

 private:
  friend class DuplicationFrameLease;
  friend struct DxgiCaptureSourceTestAccess;
  struct SharedDuplicationState;

  DxgiCaptureSource(std::shared_ptr<SharedDuplicationState> state, QpcClock& clock);

  std::shared_ptr<SharedDuplicationState> state_;
  QpcClock& clock_;
  QpcTicks last_timestamp_{0};
};

}  // namespace rebelliocap
