#pragma once

#include <d3d11.h>
#include <dxgicommon.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "core/result.h"
#include "video/frame_converter.h"

namespace rebelliocap {

struct D3d11Nv12ConverterConfig {
  std::uint32_t input_width;
  std::uint32_t input_height;
  std::uint32_t output_width;
  std::uint32_t output_height;
  std::size_t output_pool_size;
  DXGI_MODE_ROTATION rotation{DXGI_MODE_ROTATION_IDENTITY};
};

struct D3d11Nv12ProcessorState {
  DXGI_COLOR_SPACE_TYPE input_color_space;
  DXGI_COLOR_SPACE_TYPE output_color_space;
  bool auto_processing_enabled;
};

struct D3d11Nv12RuntimeMetrics {
  std::uint64_t input_views_created{0};
  std::uint64_t processing_submissions{0};
  std::uint64_t cached_conversions{0};
};

class D3d11Nv12Converter final : public IFrameConverter {
 public:
  static Result<std::unique_ptr<D3d11Nv12Converter>> create(
      ID3D11Device* device, ID3D11DeviceContext* context,
      const D3d11Nv12ConverterConfig& config);

  // Capture-worker only; externally serialized with convert/config/metrics.
  // Preserves the output pool and active encoder leases; failure is transactional.
  Result<void> reconfigure_input(std::uint32_t width, std::uint32_t height,
      DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY);

  ~D3d11Nv12Converter() override;
  D3d11Nv12Converter(const D3d11Nv12Converter&) = delete;
  D3d11Nv12Converter& operator=(const D3d11Nv12Converter&) = delete;

  Result<ConvertedVideoFrame> convert(const CapturedVideoFrame& frame) override;

  // The caller must change content_revision whenever it writes this texture.
  // Only an unleased output surface with the same contents can be reused;
  // NVENC retains exclusive ownership of every in-flight pool surface.
  Result<ConvertedVideoFrame> convert_cached(const CapturedVideoFrame& frame,
                                            std::uint64_t content_revision);
  [[nodiscard]] D3d11Nv12RuntimeMetrics metrics();

  // Queries the actual video-processor state through ID3D11VideoContext1.
  [[nodiscard]] D3d11Nv12ProcessorState processor_state();
  [[nodiscard]] const D3d11Nv12ConverterConfig& config() const noexcept;

 private:
  struct Impl;

  explicit D3d11Nv12Converter(std::unique_ptr<Impl> implementation) noexcept;
  Result<ConvertedVideoFrame> convert_impl(const CapturedVideoFrame& frame,
                                           std::optional<std::uint64_t> revision);

  std::unique_ptr<Impl> implementation_;
};

}  // namespace rebelliocap
