#pragma once

#include <Windows.h>
#include <dxgi.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/result.h"

namespace rebelliocap {

struct MonitorId {
  LUID adapter_luid;
  std::uint32_t output_index;
};

struct MonitorInfo {
  MonitorId id;
  std::wstring name;
  RECT desktop_rect;
  DXGI_MODE_ROTATION rotation;
  bool primary;
};

Result<std::vector<MonitorInfo>> enumerate_monitors();

}  // namespace rebelliocap
