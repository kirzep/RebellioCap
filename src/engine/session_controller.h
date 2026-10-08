#pragma once
#include <future>
#include <istream>
#include <ostream>
#include <unordered_set>
#include "engine/recorder_engine.h"
#include "engine/session_protocol.h"
#include "cli/arguments.h"

namespace rebelliocap {
struct HostConfiguration {
  EngineConfig engine;
  std::optional<std::filesystem::path> test_output;
  std::uint32_t duration_seconds{5};
};
Result<HostConfiguration> decode_host_configuration(std::string_view text, bool test);

struct BoundedRecordingResult {
  Result<std::filesystem::path> saved;
  Result<void> stopped;
  EngineMetrics metrics;
};

BoundedRecordingResult finish_bounded_recording(RecorderEngine& engine,
                                                 QpcTicks requested_at);

// Single-owner controller. Runtime commands and poll are serialized by the host loop.
class SessionController {
 public:
  explicit SessionController(RecorderEngineDependencies dependencies);
  Result<void> start(const EngineConfig& config);
  Result<void> handle(const SessionRequest& request);
  Result<void> stop();
  void poll(bool publish_snapshot = false);
  EngineSnapshot snapshot() const { return snapshot_; }
  std::vector<SessionEvent> take_events();
 private:
  void publish(SessionEventType type, std::optional<std::string> id = {});
  void refresh();
  void fail(const Error& error);
  std::shared_ptr<IEngineClock> clock_;
  RecorderEngine engine_;
  EngineSnapshot snapshot_{0, EngineLifecycle::Stopped, false, false, 0, {}, {}};
  bool started_{false};
  std::unordered_set<std::string> request_ids_;
  struct PendingSave { std::string id; std::future<Result<std::filesystem::path>> future; };
  std::vector<PendingSave> saves_;
  std::vector<SessionEvent> events_;
};
CliExitCode run_native_host(const CliArguments& arguments, std::ostream& output);
}
