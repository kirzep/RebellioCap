#include "engine/session_protocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace rebelliocap {
namespace {

using nlohmann::json;

Error protocol_error(std::string code, std::string message) {
  return {.code = std::move(code), .message = std::move(message), .hresult = std::nullopt};
}

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<std::uint8_t>(text[index]);
    if (lead <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if (lead >= 0xC2U && lead <= 0xDFU) {
      continuation_count = 1;
      code_point = lead & 0x1FU;
      minimum = 0x80U;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      continuation_count = 2;
      code_point = lead & 0x0FU;
      minimum = 0x800U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      continuation_count = 3;
      code_point = lead & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }

    if (continuation_count > text.size() - index - 1) return false;
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto continuation = static_cast<std::uint8_t>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) return false;
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (code_point < minimum || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
}

bool is_request_id_character(unsigned char character) noexcept {
  return (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z') ||
         (character >= '0' && character <= '9') || character == '_' || character == '-';
}

bool is_valid_request_id(std::string_view request_id) noexcept {
  return !request_id.empty() && request_id.size() <= 64U &&
         std::all_of(request_id.begin(), request_id.end(), [](char character) {
           return is_request_id_character(static_cast<unsigned char>(character));
         });
}

std::optional<SessionCommand> session_command(std::string_view command) noexcept {
  if (command == "save_replay") return SessionCommand::SaveReplay;
  if (command == "toggle_recording") return SessionCommand::ToggleRecording;
  if (command == "start_replay") return SessionCommand::StartReplay;
  if (command == "stop_replay") return SessionCommand::StopReplay;
  if (command == "stop") return SessionCommand::Stop;
  if (command == "get_snapshot") return SessionCommand::GetSnapshot;
  if (command == "reload_recording_names") return SessionCommand::ReloadRecordingNames;
  return std::nullopt;
}

const char* event_type_name(SessionEventType type) noexcept {
  switch (type) {
    case SessionEventType::Ready: return "ready";
    case SessionEventType::Snapshot: return "snapshot";
    case SessionEventType::CommandResult: return "command_result";
    case SessionEventType::ClipSaved: return "clip_saved";
    case SessionEventType::Error: return "error";
    case SessionEventType::FatalError: return "fatal_error";
  }
  return "error";
}

const char* lifecycle_name(EngineLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case EngineLifecycle::Starting: return "starting";
    case EngineLifecycle::Ready: return "ready";
    case EngineLifecycle::Stopped: return "stopped";
    case EngineLifecycle::Recovering: return "recovering";
    case EngineLifecycle::Degraded: return "degraded";
    case EngineLifecycle::Blocked: return "blocked";
    case EngineLifecycle::Failed: return "failed";
  }
  return "failed";
}

json error_json(const Error& error) {
  json value{{"code", error.code}, {"message", error.message}};
  if (error.hresult.has_value()) value["hresult"] = *error.hresult;
  return value;
}

json metrics_json(const EngineMetrics& metrics) {
  return {{"videoTicks", metrics.video_ticks},
          {"missedVideoDeadlines", metrics.missed_video_deadlines},
          {"videoPackets", metrics.video_packets},
          {"audioPackets", metrics.audio_packets},
          {"audioMixingDroppedFrames", metrics.audio_mixing_dropped_frames},
          {"audioUnavailableSources", metrics.audio_unavailable_sources},
          {"saveRequests", metrics.save_requests},
          {"completedSaves", metrics.completed_saves},
          {"failedSaves", metrics.failed_saves},
          {"rejectedSaves", metrics.rejected_saves},
          {"pipelineErrors", metrics.pipeline_errors},
          {"continuousPackets", metrics.continuous_packets},
          {"continuousFailures", metrics.continuous_failures},
          {"continuousRecordingActive", metrics.continuous_recording_active},
          {"lastHotkeySaveLatencyTicks", metrics.last_hotkey_save_latency_ticks},
          {"replayBytes", metrics.replay_bytes},
          {"budgetBytes", metrics.budget_bytes},
          {"bufferedBytes", metrics.buffered_bytes},
          {"replayRetainedSeconds", metrics.replay_retained_seconds},
          {"replayMemoryLimited", metrics.replay_memory_limited},
          {"replayMemoryDrops", metrics.replay_memory_drops},
          {"continuousRecoveryPath", metrics.continuous_recovery_path}};
}

json snapshot_json(const EngineSnapshot& snapshot) {
  json value{{"revision", snapshot.revision},
             {"lifecycle", lifecycle_name(snapshot.lifecycle)},
             {"replayActive", snapshot.replay_active},
             {"continuousRecordingActive", snapshot.continuous_recording_active},
             {"replaySeconds", snapshot.replay_seconds},
             {"metrics", metrics_json(snapshot.metrics)}};
  if (snapshot.last_error.has_value()) value["lastError"] = error_json(*snapshot.last_error);
  return value;
}

}  // namespace

Result<SessionRequest> decode_session_request(
    std::string_view line, std::optional<std::string>* validated_request_id) {
  if (validated_request_id != nullptr) validated_request_id->reset();
  if (line.size() > kMaximumSessionMessageBytes) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.message_too_large", "The protocol message exceeds 64 KiB."));
  }
  if (line.find('\0') != std::string_view::npos) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.embedded_nul", "The protocol message contains an embedded NUL."));
  }
  if (!is_valid_utf8(line)) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.invalid_utf8", "The protocol message is not valid UTF-8."));
  }

  if (line.ends_with("\r\n")) {
    line.remove_suffix(2);
  } else if (line.ends_with('\r') || line.ends_with('\n')) {
    line.remove_suffix(1);
  }
  if (line.empty() || line.find_first_of("\r\n") != std::string_view::npos) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.invalid_line_ending", "The protocol message must contain one JSON line."));
  }

  bool duplicate_key = false;
  std::size_t root_request_id_keys = 0;
  std::vector<std::unordered_set<std::string>> object_key_scopes;
  json request;
  try {
    request = json::parse(line.begin(), line.end(),
                          [&duplicate_key, &root_request_id_keys, &object_key_scopes](
                              int, json::parse_event_t event, json& value) {
      if (event == json::parse_event_t::object_start) {
        object_key_scopes.emplace_back();
      } else if (event == json::parse_event_t::key) {
        if (object_key_scopes.size() == 1U && value.get<std::string>() == "requestId") {
          ++root_request_id_keys;
        }
        auto& keys = object_key_scopes.back();
        duplicate_key = duplicate_key || !keys.insert(value.get<std::string>()).second;
      } else if (event == json::parse_event_t::object_end) {
        object_key_scopes.pop_back();
      }
      return true;
    });
  } catch (const json::exception&) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.invalid_json", "The protocol message is not valid JSON."));
  }
  if (!request.is_object()) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.invalid_request", "The protocol request must be a JSON object."));
  }

  const auto correlated_id = request.find("requestId");
  if (validated_request_id != nullptr && root_request_id_keys == 1U &&
      correlated_id != request.end() && correlated_id->is_string()) {
    const auto candidate = correlated_id->get<std::string>();
    if (is_valid_request_id(candidate)) *validated_request_id = candidate;
  }
  if (duplicate_key) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.duplicate_key", "The protocol message contains a duplicate JSON key."));
  }

  constexpr std::array<std::string_view, 4> kFields{
      "protocolVersion", "requestId", "type", "command"};
  if (request.size() != kFields.size()) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.unrecognized_field", "The protocol request contains unrecognized fields."));
  }
  for (const auto& [key, value] : request.items()) {
    (void)value;
    if (std::find(kFields.begin(), kFields.end(), key) == kFields.end()) {
      return Result<SessionRequest>::failure(
          protocol_error("protocol.unrecognized_field", "The protocol request contains unrecognized fields."));
    }
  }

  const auto protocol_version = request.find("protocolVersion");
  const auto request_id = request.find("requestId");
  const auto type = request.find("type");
  const auto command = request.find("command");
  if (protocol_version == request.end() || request_id == request.end() || type == request.end() ||
      command == request.end() || !protocol_version->is_number_unsigned() ||
      !request_id->is_string() || !type->is_string() || !command->is_string()) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.invalid_request", "The protocol request has invalid fields."));
  }
  const auto protocol_version_value = protocol_version->get<std::uint64_t>();
  if (protocol_version_value > (std::numeric_limits<std::uint32_t>::max)() ||
      protocol_version_value != kSessionProtocolVersion) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.unsupported_version", "The protocol version is not supported."));
  }
  const auto request_id_value = request_id->get<std::string>();
  if (!is_valid_request_id(request_id_value)) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.invalid_request_id", "The request ID must be 1 to 64 ASCII identifier characters."));
  }
  if (type->get<std::string>() != "command") {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.invalid_type", "The protocol request type must be command."));
  }
  const auto decoded_command = session_command(command->get<std::string>());
  if (!decoded_command.has_value()) {
    return Result<SessionRequest>::failure(
        protocol_error("protocol.unknown_command", "The protocol command is not recognized."));
  }

  return Result<SessionRequest>::success(
      {.protocol_version = kSessionProtocolVersion,
       .request_id = std::move(request_id_value),
       .command = *decoded_command});
}

std::string encode_session_event(const SessionEvent& event) {
  json value{{"protocolVersion", kSessionProtocolVersion}, {"type", event_type_name(event.type)}};
  if (event.request_id.has_value()) value["requestId"] = *event.request_id;
  if (event.snapshot.has_value()) value["snapshot"] = snapshot_json(*event.snapshot);
  if (event.error.has_value()) value["error"] = error_json(*event.error);
  if (event.output_path.has_value()) {
    const auto path = event.output_path->u8string();
    value["outputPath"] = std::string(path.begin(), path.end());
  }
  return value.dump(-1, ' ', false, json::error_handler_t::replace) + '\n';
}

}  // namespace rebelliocap
