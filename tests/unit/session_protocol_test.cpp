#include <array>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "engine/session_protocol.h"

namespace {

using namespace rebelliocap;

TEST_CASE("session protocol decodes a bounded command") {
  const auto result = decode_session_request(
      R"({"protocolVersion":1,"requestId":"r-1","type":"command","command":"save_replay"})");

  REQUIRE(result.is_success());
  REQUIRE(result.value().protocol_version == kSessionProtocolVersion);
  REQUIRE(result.value().request_id == "r-1");
  REQUIRE(result.value().command == SessionCommand::SaveReplay);
}

TEST_CASE("session protocol accepts each declared command") {
  const std::array commands{
      std::pair{"save_replay", SessionCommand::SaveReplay},
      std::pair{"toggle_recording", SessionCommand::ToggleRecording},
      std::pair{"start_replay", SessionCommand::StartReplay},
      std::pair{"stop_replay", SessionCommand::StopReplay},
      std::pair{"stop", SessionCommand::Stop},
      std::pair{"get_snapshot", SessionCommand::GetSnapshot},
      std::pair{"reload_recording_names", SessionCommand::ReloadRecordingNames},
  };

  for (const auto& [command, expected] : commands) {
    const auto request = std::string{"{\"protocolVersion\":1,\"requestId\":\"r-2\",\"type\":\"command\",\"command\":\""} +
                         command + "\"}";
    const auto result = decode_session_request(request);

    REQUIRE(result.is_success());
    REQUIRE(result.value().command == expected);
  }
}

TEST_CASE("session protocol rejects invalid command envelopes") {
  const std::array requests{
      R"({"protocolVersion":2,"requestId":"r-1","type":"command","command":"save_replay"})",
      R"({"protocolVersion":4294967297,"requestId":"r-1","type":"command","command":"save_replay"})",
      R"({"protocolVersion":1,"requestId":"r-1","type":"command","command":"erase_everything"})",
      R"({"protocolVersion":1,"type":"command","command":"save_replay"})",
      R"({"protocolVersion":1,"requestId":"r-1","type":"command","command":"save_replay","extra":true})",
      R"({"protocolVersion":1,"requestId":"r-1","type":"command","command":"save_replay","command":"stop"})",
      R"({"protocolVersion":{},"protocolVersion":1,"requestId":"r-1","type":"command","command":"stop"})",
      R"({"protocolVersion":1,"requestId":"r-1","type":"command","command":"save_replay"} garbage)",
  };

  for (const auto request : requests) {
    REQUIRE_FALSE(decode_session_request(request).is_success());
  }
}

TEST_CASE("rejected request retains only an unambiguous validated request ID") {
  std::optional<std::string> request_id;
  auto unknown = decode_session_request(
      R"({"protocolVersion":1,"requestId":"pending-7","type":"command","command":"erase_everything"})",
      &request_id);
  REQUIRE_FALSE(unknown.is_success());
  REQUIRE(unknown.error().code == "protocol.unknown_command");
  REQUIRE(request_id == "pending-7");

  auto unsupported = decode_session_request(
      R"({"protocolVersion":2,"requestId":"pending-8","type":"command","command":"stop"})",
      &request_id);
  REQUIRE_FALSE(unsupported.is_success());
  REQUIRE(unsupported.error().code == "protocol.unsupported_version");
  REQUIRE(request_id == "pending-8");

  for (const auto request : {
           R"({"protocolVersion":1,"type":"command","command":"stop"})",
           R"({"protocolVersion":1,"requestId":"not valid","type":"command","command":"stop"})",
           R"({"protocolVersion":1,"requestId":"first","requestId":"second","type":"command","command":"stop"})"}) {
    auto rejected = decode_session_request(request, &request_id);
    REQUIRE_FALSE(rejected.is_success());
    REQUIRE_FALSE(request_id.has_value());
  }
}

TEST_CASE("session protocol rejects invalid line and request identifier boundaries") {
  const std::array invalid_utf8{static_cast<char>(0xC3), static_cast<char>(0x28)};
  const std::string invalid_utf8_view(invalid_utf8.data(), invalid_utf8.size());
  const std::string embedded_nul = std::string(R"({"protocolVersion":1})") + '\0';
  const std::string too_long_request_id(65, 'r');
  const std::array<std::string, 7> requests{
      invalid_utf8_view,
      embedded_nul,
      std::string(kMaximumSessionMessageBytes + 1, 'x'),
      std::string(R"({"protocolVersion":1,"requestId":"r-1","type":"command","command":"save_replay"})") + "\n\n",
      R"({"protocolVersion":1,"requestId":"r 1","type":"command","command":"save_replay"})",
      R"({"protocolVersion":1,"requestId":"","type":"command","command":"save_replay"})",
      std::string{"{\"protocolVersion\":1,\"requestId\":\""} + too_long_request_id +
          "\",\"type\":\"command\",\"command\":\"save_replay\"}",
  };

  for (const auto& request : requests) {
    const auto result = decode_session_request(request);
    REQUIRE_FALSE(result.is_success());
  }
  REQUIRE(decode_session_request(requests[2]).error().code == "protocol.message_too_large");
}

TEST_CASE("session protocol serializes snapshot and error events as one NDJSON object") {
  const EngineSnapshot snapshot{
      .revision = 7,
      .lifecycle = EngineLifecycle::Ready,
      .replay_active = true,
      .continuous_recording_active = false,
      .replay_seconds = 30,
      .metrics = {.video_ticks = 120,
                  .video_packets = 119,
                  .audio_mixing_dropped_frames = 24'000,
                  .continuous_packets = 33,
                  .continuous_failures = 2,
                  .continuous_recording_active = true,
                  .replay_bytes = 4096,
                  .budget_bytes = 8192,
                  .buffered_bytes = 5000,
                  .replay_retained_seconds = 12.5,
                  .replay_memory_limited = true,
                  .replay_memory_drops = 16,
                  .continuous_recovery_path = "C:/Clips/recovered.mp4.partial"},
      .last_error = std::nullopt,
  };
  const auto snapshot_line = encode_session_event(
      {.type = SessionEventType::Snapshot, .snapshot = snapshot});
  const auto snapshot_json = nlohmann::json::parse(snapshot_line);

  REQUIRE(snapshot_line.ends_with('\n'));
  REQUIRE(snapshot_json.at("protocolVersion") == 1);
  REQUIRE(snapshot_json.at("type") == "snapshot");
  REQUIRE(snapshot_json.at("snapshot").at("revision") == 7);
  REQUIRE(snapshot_json.at("snapshot").at("lifecycle") == "ready");
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("videoTicks") == 120);
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("audioMixingDroppedFrames") == 24'000);
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("continuousPackets") == 33);
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("continuousFailures") == 2);
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("continuousRecoveryPath") == "C:/Clips/recovered.mp4.partial");
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("continuousRecordingActive").is_boolean());
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("continuousRecordingActive") == true);
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("budgetBytes").is_number_unsigned());
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("bufferedBytes").is_number_unsigned());
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("budgetBytes") == 8192);
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("bufferedBytes") == 5000);
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("replayRetainedSeconds") == 12.5);
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("replayMemoryLimited") == true);
  REQUIRE(snapshot_json.at("snapshot").at("metrics").at("replayMemoryDrops") == 16);
  REQUIRE_FALSE(snapshot_json.at("snapshot").contains("lastError"));

  const auto error_line = encode_session_event({
      .type = SessionEventType::Error,
      .request_id = "r-7",
      .error = Error{.code = "engine.blocked", .message = "The engine is blocked.", .hresult = 5},
  });
  const auto error_json = nlohmann::json::parse(error_line);

  REQUIRE(error_line.ends_with('\n'));
  REQUIRE(error_json.at("type") == "error");
  REQUIRE(error_json.at("requestId") == "r-7");
  REQUIRE(error_json.at("error").at("code") == "engine.blocked");
  REQUIRE(error_json.at("error").at("hresult") == 5);
}

}  // namespace
