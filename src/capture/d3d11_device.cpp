#include "capture/d3d11_device.h"

#include <dxgi1_2.h>

#include <array>
#include <string>

namespace rebelliocap {
namespace {

bool luid_equal(LUID left, LUID right) noexcept {
  return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

Error hresult_error(std::string code, std::string message, HRESULT result) {
  return Error{std::move(code), std::move(message), static_cast<long>(result)};
}

}  // namespace

Result<D3d11Device> create_d3d11_device(LUID adapter_luid) {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(result)) {
    return Result<D3d11Device>::failure(hresult_error(
        "d3d11.factory_failed", "Could not create the DXGI factory.", result));
  }

  Microsoft::WRL::ComPtr<IDXGIAdapter1> selected_adapter;
  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    result = factory->EnumAdapters1(index, &adapter);
    if (result == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(result)) {
      return Result<D3d11Device>::failure(hresult_error(
          "d3d11.adapter_enumeration_failed", "Could not enumerate DXGI adapters.", result));
    }

    DXGI_ADAPTER_DESC1 description{};
    result = adapter->GetDesc1(&description);
    if (FAILED(result)) {
      return Result<D3d11Device>::failure(hresult_error(
          "d3d11.adapter_description_failed", "Could not inspect a DXGI adapter.", result));
    }
    if (luid_equal(description.AdapterLuid, adapter_luid)) {
      selected_adapter = std::move(adapter);
      break;
    }
  }

  if (selected_adapter == nullptr) {
    return Result<D3d11Device>::failure(
        Error{"d3d11.adapter_not_found", "The selected adapter LUID is unavailable.", std::nullopt});
  }

  constexpr std::array feature_levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3d11Device created;
  result = D3D11CreateDevice(selected_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels.data(),
                             static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION,
                             &created.device_, nullptr, &created.context_);
  if (result == E_INVALIDARG) {
    result = D3D11CreateDevice(selected_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels.data() + 1, 1,
                               D3D11_SDK_VERSION, &created.device_, nullptr,
                               &created.context_);
  }
  if (FAILED(result)) {
    return Result<D3d11Device>::failure(hresult_error(
        "d3d11.device_creation_failed", "Could not create the D3D11 capture device.", result));
  }

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  result = created.device_.As(&dxgi_device);
  if (FAILED(result)) {
    return Result<D3d11Device>::failure(hresult_error(
        "d3d11.dxgi_device_failed", "Could not query the D3D11 DXGI device.", result));
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter> actual_adapter;
  result = dxgi_device->GetAdapter(&actual_adapter);
  if (FAILED(result)) {
    return Result<D3d11Device>::failure(hresult_error(
        "d3d11.actual_adapter_failed", "Could not query the D3D11 device adapter.", result));
  }
  DXGI_ADAPTER_DESC actual_description{};
  result = actual_adapter->GetDesc(&actual_description);
  if (FAILED(result) || !luid_equal(actual_description.AdapterLuid, adapter_luid)) {
    return Result<D3d11Device>::failure(hresult_error(
        "d3d11.adapter_mismatch", "D3D11 created the device on a different adapter.", result));
  }

  created.adapter_luid_ = actual_description.AdapterLuid;
  return Result<D3d11Device>::success(std::move(created));
}

}  // namespace rebelliocap
