#include "audio/audio_endpoint_catalog.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace rebelliocap {
namespace {

using Microsoft::WRL::ComPtr;
using audio_endpoint_catalog_detail::EndpointRecord;

Error catalog_error(std::string code, std::string message, HRESULT result) {
  return Error{std::move(code), std::move(message), static_cast<long>(result)};
}

struct CoTaskMemDeleter {
  void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); }
};

using CoTaskMemString = std::unique_ptr<wchar_t, CoTaskMemDeleter>;

class ScopedPropVariant {
 public:
  ScopedPropVariant() noexcept { PropVariantInit(&value_); }
  ~ScopedPropVariant() { PropVariantClear(&value_); }

  ScopedPropVariant(const ScopedPropVariant&) = delete;
  ScopedPropVariant& operator=(const ScopedPropVariant&) = delete;

  [[nodiscard]] PROPVARIANT* receive() noexcept { return &value_; }
  [[nodiscard]] const PROPVARIANT& value() const noexcept { return value_; }

 private:
  PROPVARIANT value_{};
};

}  // namespace

namespace audio_endpoint_catalog_detail {

std::vector<AudioEndpointInfo> project_endpoints(
    const std::vector<EndpointRecord>& records, std::wstring_view default_id) {
  std::vector<AudioEndpointInfo> endpoints;
  endpoints.reserve(records.size());

  bool default_marked = false;
  for (const auto& record : records) {
    if (record.state != DEVICE_STATE_ACTIVE) {
      continue;
    }

    const bool is_default = !default_marked && record.id == default_id;
    endpoints.push_back(AudioEndpointInfo{record.id, record.name, is_default});
    default_marked = default_marked || is_default;
  }

  std::stable_sort(endpoints.begin(), endpoints.end(),
                   [](const AudioEndpointInfo& left, const AudioEndpointInfo& right) {
                     if (left.default_console != right.default_console) {
                       return left.default_console;
                     }
                     if (left.name != right.name) {
                       return left.name < right.name;
                     }
                     return left.id < right.id;
                   });
  return endpoints;
}

std::vector<AudioEndpointInfo> project_microphones(
    const std::vector<EndpointRecord>& records, std::wstring_view default_id) {
  return project_endpoints(records, default_id);
}

std::vector<AudioEndpointInfo> project_system_audio(
    const std::vector<EndpointRecord>& records, std::wstring_view default_id) {
  return project_endpoints(records, default_id);
}

}  // namespace audio_endpoint_catalog_detail

Result<std::vector<AudioEndpointInfo>> enumerate_endpoints(EDataFlow flow) {
  using ResultType = Result<std::vector<AudioEndpointInfo>>;

  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
  if (FAILED(result)) {
    return ResultType::failure(catalog_error(
        "audio.catalog_enumerator_failed", "Could not create the audio endpoint enumerator.",
        result));
  }

  ComPtr<IMMDeviceCollection> collection;
  result = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(result)) {
    return ResultType::failure(catalog_error(
        "audio.catalog_enumeration_failed", "Could not enumerate active audio endpoints.",
        result));
  }

  UINT endpoint_count = 0;
  result = collection->GetCount(&endpoint_count);
  if (FAILED(result)) {
    return ResultType::failure(catalog_error(
        "audio.catalog_count_failed", "Could not count active audio endpoints.", result));
  }
  if (endpoint_count == 0) {
    return ResultType::success({});
  }

  ComPtr<IMMDevice> default_endpoint;
  result = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &default_endpoint);
  if (FAILED(result)) {
    return ResultType::failure(catalog_error(
        "audio.catalog_default_failed", "Could not obtain the default console audio endpoint.",
        result));
  }

  wchar_t* raw_default_id = nullptr;
  result = default_endpoint->GetId(&raw_default_id);
  CoTaskMemString default_id(raw_default_id);
  if (FAILED(result)) {
    return ResultType::failure(catalog_error(
        "audio.catalog_default_id_failed", "Could not read the default audio endpoint ID.",
        result));
  }
  if (default_id == nullptr || default_id.get()[0] == L'\0') {
    return ResultType::failure(Error{"audio.catalog_default_id_invalid",
                                     "The default audio endpoint has no stable ID.",
                                     std::nullopt});
  }

  std::vector<EndpointRecord> records;
  records.reserve(endpoint_count);
  for (UINT index = 0; index < endpoint_count; ++index) {
    ComPtr<IMMDevice> endpoint;
    result = collection->Item(index, &endpoint);
    if (FAILED(result)) {
      return ResultType::failure(catalog_error(
          "audio.catalog_item_failed", "Could not access an audio endpoint.", result));
    }

    DWORD state = 0;
    result = endpoint->GetState(&state);
    if (FAILED(result)) {
      return ResultType::failure(catalog_error(
          "audio.catalog_state_failed", "Could not read an audio endpoint state.", result));
    }

    wchar_t* raw_endpoint_id = nullptr;
    result = endpoint->GetId(&raw_endpoint_id);
    CoTaskMemString endpoint_id(raw_endpoint_id);
    if (FAILED(result)) {
      return ResultType::failure(catalog_error(
          "audio.catalog_id_failed", "Could not read an audio endpoint ID.", result));
    }
    if (endpoint_id == nullptr || endpoint_id.get()[0] == L'\0') {
      return ResultType::failure(Error{"audio.catalog_id_invalid",
                                       "An audio endpoint has no stable ID.", std::nullopt});
    }

    ComPtr<IPropertyStore> properties;
    result = endpoint->OpenPropertyStore(STGM_READ, &properties);
    if (FAILED(result)) {
      return ResultType::failure(catalog_error(
          "audio.catalog_properties_failed", "Could not open audio endpoint properties.",
          result));
    }

    ScopedPropVariant friendly_name;
    result = properties->GetValue(PKEY_Device_FriendlyName, friendly_name.receive());
    if (FAILED(result)) {
      return ResultType::failure(catalog_error(
          "audio.catalog_name_failed", "Could not read an audio endpoint friendly name.",
          result));
    }
    if (friendly_name.value().vt != VT_LPWSTR || friendly_name.value().pwszVal == nullptr ||
        friendly_name.value().pwszVal[0] == L'\0') {
      return ResultType::failure(Error{"audio.catalog_name_invalid",
                                       "An audio endpoint has no user-facing friendly name.",
                                       std::nullopt});
    }

    records.push_back(
        EndpointRecord{endpoint_id.get(), friendly_name.value().pwszVal, state});
  }

  return ResultType::success(audio_endpoint_catalog_detail::project_endpoints(
      records, default_id.get()));
}

Result<std::vector<AudioEndpointInfo>> enumerate_microphones() {
  return enumerate_endpoints(eCapture);
}

Result<std::vector<AudioEndpointInfo>> enumerate_system_audio() {
  return enumerate_endpoints(eRender);
}

}  // namespace rebelliocap
