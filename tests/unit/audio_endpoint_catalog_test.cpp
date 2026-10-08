#include <Catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "audio/audio_endpoint_catalog.h"
#include "audio/wasapi_capture_source.h"

namespace rebelliocap {
namespace {

using audio_endpoint_catalog_detail::EndpointRecord;
using audio_endpoint_catalog_detail::project_microphones;
using audio_endpoint_catalog_detail::project_system_audio;

TEST_CASE("microphone catalog keeps only active endpoints and preserves Unicode") {
  const std::vector<EndpointRecord> records{
      {L"disabled", L"Old Mic", DEVICE_STATE_DISABLED},
      {L"ambiguous", L"Changing Mic", DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED},
      {L"usb-2", L"Микрофон Б", DEVICE_STATE_ACTIVE},
      {L"usb-1", L"Microphone A", DEVICE_STATE_ACTIVE}};

  const auto result = project_microphones(records, L"usb-2");

  REQUIRE(result.size() == 2);
  CHECK(result[0] == AudioEndpointInfo{L"usb-2", L"Микрофон Б", true});
  CHECK(result[1].id == L"usb-1");
  CHECK(result[1].name == L"Microphone A");
  CHECK_FALSE(result[1].default_console);
}

TEST_CASE("microphone catalog sorts by default then name then endpoint id") {
  const std::vector<EndpointRecord> records{
      {L"z-id", L"Same", DEVICE_STATE_ACTIVE},
      {L"b-id", L"Zulu", DEVICE_STATE_ACTIVE},
      {L"a-id", L"Same", DEVICE_STATE_ACTIVE},
      {L"default-id", L"zzz default", DEVICE_STATE_ACTIVE}};

  const auto result = project_microphones(records, L"default-id");

  REQUIRE(result.size() == 4);
  CHECK(result[0].id == L"default-id");
  CHECK(result[1].id == L"a-id");
  CHECK(result[2].id == L"z-id");
  CHECK(result[3].id == L"b-id");
}

TEST_CASE("microphone catalog marks the default endpoint at most once") {
  const std::vector<EndpointRecord> records{
      {L"default-id", L"First", DEVICE_STATE_ACTIVE},
      {L"default-id", L"Second", DEVICE_STATE_ACTIVE},
      {L"other-id", L"Other", DEVICE_STATE_ACTIVE}};

  const auto result = project_microphones(records, L"default-id");
  const auto default_count = std::count_if(
      result.begin(), result.end(), [](const AudioEndpointInfo& endpoint) {
        return endpoint.default_console;
      });

  CHECK(default_count == 1);
}

TEST_CASE("system audio catalog exposes active render endpoints with a stable default ID") {
  const std::vector<EndpointRecord> records{
      {L"render-z", L"Speakers", DEVICE_STATE_ACTIVE},
      {L"render-a", L"Display Audio", DEVICE_STATE_ACTIVE},
      {L"render-disabled", L"Old Output", DEVICE_STATE_DISABLED}};

  const auto endpoints = project_system_audio(records, L"render-z");

  REQUIRE(endpoints == std::vector<AudioEndpointInfo>{
                           {L"render-z", L"Speakers", true},
                           {L"render-a", L"Display Audio", false}});
}

TEST_CASE("WASAPI requires a pinned stable endpoint ID") {
  QpcClock clock;
  const auto result = WasapiCaptureSource::create(
      {AudioEndpointRole::SystemLoopback, std::nullopt}, clock);

  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "audio.endpoint_required");
}

}  // namespace
}  // namespace rebelliocap
