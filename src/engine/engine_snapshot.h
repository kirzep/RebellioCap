#pragma once

#include <cstdint>
#include <optional>

#include "core/error.h"
#include "engine/engine_config.h"

namespace rebelliocap {

enum class EngineLifecycle {
  Starting,
  Ready,
  Stopped,
  Recovering,
  Degraded,
  Blocked,
  Failed,
};

struct EngineSnapshot {
  std::uint64_t revision;
  EngineLifecycle lifecycle;
  bool replay_active;
  bool continuous_recording_active;
  std::uint32_t replay_seconds;
  EngineMetrics metrics;
  std::optional<Error> last_error;
};

}  // namespace rebelliocap
