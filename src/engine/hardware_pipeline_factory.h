#pragma once
#include <nlohmann/json.hpp>
#include "engine/recorder_engine.h"
#include "cli/arguments.h"
#include "core/diagnostic_writer.h"
namespace rebelliocap {
class HardwarePipelineFactory {
 public:
  Result<RecorderEngineDependencies> create(const EngineConfig& config);
  Result<nlohmann::json> catalog();
  nlohmann::json doctor(std::optional<MonitorId> monitor = std::nullopt);
  CliExitCode run_legacy(const CliArguments& arguments, DiagnosticWriter& writer);
 private:
  QpcClock clock_;
};
}
