#include "capture/monitor_catalog.h"

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <utility>
#include <cwctype>
#include <algorithm>

namespace rebelliocap {
namespace {

Error hresult_error(std::string code, std::string message, HRESULT result) {
  return Error{std::move(code), std::move(message), static_cast<long>(result)};
}

// Resolve the device interface rather than persisting boot-scoped adapter LUIDs.
std::wstring monitor_device_path(const wchar_t* gdi_name) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    const auto status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                                          &mode_count, modes.data(), nullptr);
    if (status == ERROR_INSUFFICIENT_BUFFER) continue;
    if (status != ERROR_SUCCESS) return {};
    std::vector<std::wstring> targets;
    for (UINT32 index = 0; index < path_count; ++index) {
      const auto& path = paths[index];
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
      source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                       path.sourceInfo.adapterId, path.sourceInfo.id};
      // An unreadable source could be another target of this clone group.
      // Do not persist an incomplete target set as a different identity.
      if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) return {};
      if (_wcsicmp(source.viewGdiDeviceName, gdi_name) != 0) continue;
      DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
      target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target),
                       path.targetInfo.adapterId, path.targetInfo.id};
      if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS || target.monitorDevicePath[0] == 0) return {};
      std::wstring candidate = target.monitorDevicePath;
      for (auto& character : candidate) character = static_cast<wchar_t>(std::towlower(character));
      targets.push_back(std::move(candidate));
    }
    return monitor_source_identity(targets);
  }
  return {};
}

}  // namespace

std::wstring monitor_source_identity(std::span<const std::wstring> target_paths) {
  std::vector<std::wstring> targets(target_paths.begin(), target_paths.end());
  for (auto& target : targets) {
    if (target.empty() || target.find(L'|') != std::wstring::npos) return {};
    for (auto& character : target) character = static_cast<wchar_t>(std::towlower(character));
  }
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  // A cloned desktop source represents every target, independent of target order.
  std::wstring identity;
  for (const auto& target : targets) {
    if (!identity.empty()) identity.push_back(L'|');
    identity += target;
  }
  return identity;
}

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
      auto device_path = monitor_device_path(description.DeviceName);
      if (device_path.empty()) {
        // An unidentified source cannot be saved safely, but must not prevent
        // capturing another source whose device identity is available.
        continue;
      }
      monitors.push_back(MonitorInfo{
          MonitorId{adapter_description.AdapterLuid, output_index, std::move(device_path)}, description.DeviceName,
          description.DesktopCoordinates, description.Rotation, primary});
    }
  }

  return Result<std::vector<MonitorInfo>>::success(std::move(monitors));
}

}  // namespace rebelliocap
