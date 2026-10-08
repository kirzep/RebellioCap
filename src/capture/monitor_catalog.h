#pragma once

#include <Windows.h>
#include <dxgi.h>

#include <cstdint>
#include <string>
#include <vector>
#include <span>

#include "core/result.h"

namespace rebelliocap {

struct MonitorId {
  LUID adapter_luid;
  std::uint32_t output_index;
  // Windows monitor device interface path; LUID and output index remain runtime addresses.
  std::wstring device_path;
};

struct MonitorInfo {
  MonitorId id;
  std::wstring name;
  RECT desktop_rect;
  DXGI_MODE_ROTATION rotation;
  bool primary;
};

Result<std::vector<MonitorInfo>> enumerate_monitors();
// A desktop clone source is identified by every physical target, canonically.
std::wstring monitor_source_identity(std::span<const std::wstring> target_paths);

}  // namespace rebelliocap
