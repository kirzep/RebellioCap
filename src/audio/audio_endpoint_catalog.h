#pragma once

#include <Windows.h>
#include <mmdeviceapi.h>

#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace rebelliocap {

struct AudioEndpointInfo {
  std::wstring id;
  std::wstring name;
  bool default_console;

  bool operator==(const AudioEndpointInfo&) const = default;
};

Result<std::vector<AudioEndpointInfo>> enumerate_microphones();
Result<std::vector<AudioEndpointInfo>> enumerate_system_audio();

namespace audio_endpoint_catalog_detail {

struct EndpointRecord {
  std::wstring id;
  std::wstring name;
  DWORD state;
};

std::vector<AudioEndpointInfo> project_microphones(
    const std::vector<EndpointRecord>& records, std::wstring_view default_id);
std::vector<AudioEndpointInfo> project_system_audio(
    const std::vector<EndpointRecord>& records, std::wstring_view default_id);

}  // namespace audio_endpoint_catalog_detail
}  // namespace rebelliocap
