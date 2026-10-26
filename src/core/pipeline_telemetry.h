#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rebelliocap {

enum class PipelineStage : std::size_t {
  SchedulerWakeLateness, CaptureAcquireCpu, CaptureCopySubmitCpu,
  ConversionSubmitCpu, NvencSubmitCpu, NvencCollectCpu, VideoTickCpu,
  PacketFanoutCpu, ReplayAppendCpu, ReplaySnapshotCpu,
  WriterQueueResidence, WriterMuxCpu, NvencSubmitToCollection, DiskWriteCpu, QueueAdmissionCpu, Count
};
inline constexpr auto pipeline_stage_count = static_cast<std::size_t>(PipelineStage::Count);
std::string_view pipeline_stage_name(PipelineStage stage) noexcept;

struct LatencySummary {
  std::uint64_t count{}, p50_us{}, p95_us{}, p99_us{}, p999_us{}, max_us{}, overflow_count{};
};

// Fixed logarithmic bins: exact integer microseconds below 16, then eight
// subdivisions per power of two (at most 12.5% bin width). Percentiles report
// bin upper bounds; overflow quantiles report the observed maximum.
// Recording never allocates. Snapshot loads are race-free but approximate
// during writes, exact when quiescent. Count is derived from sampled bins.
class LatencyHistogram {
 public:
  static constexpr std::uint64_t maximum_tracked_us = (1ULL << 24) - 1;
  void record(std::uint64_t microseconds) noexcept;
  LatencySummary snapshot() const noexcept;
  // Reset requires all producers and snapshot readers to be quiescent.
  void reset() noexcept;
 private:
  static constexpr std::size_t bucket_count = 176;
  std::array<std::atomic<std::uint64_t>, bucket_count + 1> buckets_{};
  std::atomic<std::uint64_t> maximum_{};
};

struct PipelineTelemetrySnapshot {
  bool enabled{};
  bool trace_enabled{};
  std::array<LatencySummary, pipeline_stage_count> stages{};
};

class PipelineTelemetry {
 public:
  PipelineTelemetry() noexcept;
  void configure(bool enabled, bool trace_enabled = false) noexcept;
  bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
  void record(PipelineStage stage, std::uint64_t microseconds,
              std::uint64_t correlation = 0) noexcept;
  PipelineTelemetrySnapshot snapshot() const noexcept;
  void reset() noexcept;
 private:
  std::atomic_bool enabled_{};
  std::atomic_bool trace_enabled_{};
  const std::uint64_t session_id_;
  std::array<LatencyHistogram, pipeline_stage_count> stages_{};
};

class PipelineTimingScope {
 public:
  explicit PipelineTimingScope(PipelineTelemetry& telemetry, PipelineStage stage,
                              std::uint64_t correlation = 0) noexcept;
  explicit PipelineTimingScope(PipelineTelemetry* telemetry, PipelineStage stage,
                              std::uint64_t correlation = 0) noexcept;
  ~PipelineTimingScope();
  PipelineTimingScope(const PipelineTimingScope&) = delete;
  PipelineTimingScope& operator=(const PipelineTimingScope&) = delete;
 private:
  PipelineTelemetry* telemetry_{};
  PipelineStage stage_{};
  std::uint64_t correlation_{};
  std::chrono::steady_clock::time_point start_{};
};

}  // namespace rebelliocap
