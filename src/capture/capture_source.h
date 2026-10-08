#pragma once

#include <d3d11.h>
#include <dxgicommon.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "core/qpc_clock.h"
#include "core/result.h"

namespace rebelliocap {

struct CapturedVideoFrame {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  QpcTicks captured_at;
  std::uint32_t width;
  std::uint32_t height;

  // Desktop Duplication owns the acquired resource until ReleaseFrame. Keep
  // this opaque lease alive for every GPU operation that uses texture. Merely
  // copying texture without lifetime does not extend the duplication lease.
  std::shared_ptr<const void> lifetime{};
  // Clockwise correction from the unrotated duplication surface to desktop.
  DXGI_MODE_ROTATION rotation{DXGI_MODE_ROTATION_IDENTITY};
};

class ICaptureSource {
 public:
  virtual ~ICaptureSource() = default;

  virtual Result<std::optional<CapturedVideoFrame>> next_frame(
      std::chrono::milliseconds timeout) = 0;
};

}  // namespace rebelliocap
