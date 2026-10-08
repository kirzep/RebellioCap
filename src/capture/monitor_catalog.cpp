#include "capture/monitor_catalog.h"

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <utility>

namespace rebelliocap {
namespace {

Error hresult_error(std::string code, std::string message, HRESULT result) {
  return Error{std::move(code), std::move(message), static_cast<long>(result)};
}

}  // namespace

Result<std::vector<MonitorInfo>> enumerate_monitors() {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(result)) {
    return Result<std::vector<MonitorInfo>>::failure(hresult_error(
        "monitor_catalog.factory_failed", "Could not create the DXGI factory.", result));
  }

  std::vector<MonitorInfo> monitors;
  for (UINT adapter_index = 0;; ++adapter_index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    result = factory->EnumAdapters1(adapter_index, &adapter);
    if (result == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(result)) {
      return Result<std::vector<MonitorInfo>>::failure(hresult_error(
          "monitor_catalog.adapter_enumeration_failed", "Could not enumerate DXGI adapters.",
          result));
    }

    DXGI_ADAPTER_DESC1 adapter_description{};
    result = adapter->GetDesc1(&adapter_description);
    if (FAILED(result)) {
      return Result<std::vector<MonitorInfo>>::failure(hresult_error(
          "monitor_catalog.adapter_description_failed", "Could not inspect a DXGI adapter.",
          result));
    }

    for (UINT output_index = 0;; ++output_index) {
      Microsoft::WRL::ComPtr<IDXGIOutput> output;
      result = adapter->EnumOutputs(output_index, &output);
      if (result == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(result)) {
        return Result<std::vector<MonitorInfo>>::failure(hresult_error(
            "monitor_catalog.output_enumeration_failed", "Could not enumerate DXGI outputs.",
            result));
      }

      DXGI_OUTPUT_DESC description{};
      result = output->GetDesc(&description);
      if (FAILED(result)) {
        return Result<std::vector<MonitorInfo>>::failure(hresult_error(
            "monitor_catalog.output_description_failed", "Could not inspect a DXGI output.",
            result));
      }
      if (description.AttachedToDesktop == FALSE) {
        continue;
      }

      if (description.Monitor == nullptr) {
        return Result<std::vector<MonitorInfo>>::failure(Error{
            "monitor_catalog.monitor_handle_missing",
            "An attached DXGI output did not provide a monitor handle.", std::nullopt});
      }
      MONITORINFO monitor_info{};
      monitor_info.cbSize = sizeof(monitor_info);
      if (GetMonitorInfoW(description.Monitor, &monitor_info) == FALSE) {
        const HRESULT monitor_result = HRESULT_FROM_WIN32(GetLastError());
        return Result<std::vector<MonitorInfo>>::failure(hresult_error(
            "monitor_catalog.monitor_info_failed", "Could not inspect an attached monitor.",
            monitor_result));
      }
      const bool primary = (monitor_info.dwFlags & MONITORINFOF_PRIMARY) != 0;
      monitors.push_back(MonitorInfo{
          MonitorId{adapter_description.AdapterLuid, output_index}, description.DeviceName,
          description.DesktopCoordinates, description.Rotation, primary});
    }
  }

  return Result<std::vector<MonitorInfo>>::success(std::move(monitors));
}

}  // namespace rebelliocap
