#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "core/result.h"

namespace rebelliocap {

class D3d11Device {
 public:
  [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }
  [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return context_.Get(); }
  [[nodiscard]] LUID adapter_luid() const noexcept { return adapter_luid_; }

 private:
  friend Result<D3d11Device> create_d3d11_device(LUID adapter_luid);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  LUID adapter_luid_{};
};

Result<D3d11Device> create_d3d11_device(LUID adapter_luid);

}  // namespace rebelliocap
