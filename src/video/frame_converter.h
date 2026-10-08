#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <utility>

#include "capture/capture_source.h"
#include "core/result.h"

namespace rebelliocap {

class D3d11Nv12Converter;

// Move-only by design: the private lease prevents a pooled surface from being
// reused while this frame is alive. Callers must retain this object until
// every asynchronous GPU consumer has finished accessing texture(); submitting
// work alone is not sufficient when the consumer can outlive the call.
class ConvertedVideoFrame {
 public:
  ConvertedVideoFrame(const ConvertedVideoFrame&) = delete;
  ConvertedVideoFrame& operator=(const ConvertedVideoFrame&) = delete;
  ConvertedVideoFrame(ConvertedVideoFrame&&) noexcept = default;
  ConvertedVideoFrame& operator=(ConvertedVideoFrame&&) noexcept = default;
  ~ConvertedVideoFrame() = default;

  [[nodiscard]] ID3D11Texture2D* texture() const noexcept { return texture_.Get(); }

 private:
  friend struct NvencShutdownTestAccess;
  friend class D3d11Nv12Converter;

  ConvertedVideoFrame(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
                      std::shared_ptr<const void> lifetime) noexcept
      : texture_(std::move(texture)), lifetime_(std::move(lifetime)) {}

  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  std::shared_ptr<const void> lifetime_;
};

class IFrameConverter {
 public:
  virtual ~IFrameConverter() = default;
  virtual Result<ConvertedVideoFrame> convert(const CapturedVideoFrame& frame) = 0;
};

}  // namespace rebelliocap
