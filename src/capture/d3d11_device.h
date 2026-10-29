#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "core/result.h"
#include "capture/gpu_recording_priority.h"

namespace rebelliocap {

class D3d11Device {
 public:
  [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }
  [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return context_.Get(); }
  [[nodiscard]] LUID adapter_luid() const noexcept { return adapter_luid_; }
  [[nodiscard]] const GpuRecordingPriority& recording_priority() const noexcept { return priority_; }

 private:
  friend Result<D3d11Device> create_d3d11_device(LUID adapter_luid, bool recording_priority);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  LUID adapter_luid_{};
  GpuRecordingPriority priority_;
};

Result<D3d11Device> create_d3d11_device(LUID adapter_luid, bool recording_priority = false);

}  // namespace rebelliocap
