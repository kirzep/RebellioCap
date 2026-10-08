#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/result.h"
#include "engine/engine_snapshot.h"

namespace rebelliocap {

inline constexpr std::uint32_t kSessionProtocolVersion = 1;
inline constexpr std::size_t kMaximumSessionMessageBytes = 64U * 1024U;

enum class SessionCommand {
  SaveReplay,
  ToggleRecording,
  StartReplay,
  StopReplay,
  Stop,
  GetSnapshot,
  ReloadRecordingNames,
};

struct SessionRequest {
  std::uint32_t protocol_version;
  std::string request_id;
  SessionCommand command;
};

enum class SessionEventType {
  Ready,
  Snapshot,
  CommandResult,
  ClipSaved,
  Error,
  FatalError,
};

struct SessionEvent {
  SessionEventType type;
  std::optional<std::string> request_id;
  std::optional<EngineSnapshot> snapshot;
  std::optional<Error> error;
  std::optional<std::filesystem::path> output_path;
};

[[nodiscard]] Result<SessionRequest> decode_session_request(
    std::string_view line,
    std::optional<std::string>* validated_request_id = nullptr);
[[nodiscard]] std::string encode_session_event(const SessionEvent& event);

}  // namespace rebelliocap
