#include "core/pipeline_telemetry.h"

#include <bit>
#include <Windows.h>
#include <TraceLoggingProvider.h>

TRACELOGGING_DEFINE_PROVIDER(g_pipeline_provider, "RebellioCap.Pipeline",
    (0xfde54cc8, 0xf30b, 0x4d1b, 0xa1, 0x15, 0x61, 0x02, 0x90, 0x13, 0x31, 0x55));

namespace rebelliocap {
namespace {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
std::atomic<std::uint64_t> next_session{1};
struct TraceRegistration {
  TraceRegistration() noexcept { static_cast<void>(TraceLoggingRegister(g_pipeline_provider)); }
  ~TraceRegistration() { TraceLoggingUnregister(g_pipeline_provider); }
};
std::size_t bucket_for(std::uint64_t value) noexcept {
  if (value < 16) return static_cast<std::size_t>(value);
  const auto exponent = static_cast<unsigned>(std::bit_width(value) - 1);
  return 16 + (exponent - 4) * 8 + static_cast<std::size_t>((value - (1ULL << exponent)) >> (exponent - 3));
}
std::uint64_t upper_bound(std::size_t bucket) noexcept {
  if (bucket < 16) return bucket;
  const auto exponent = 4 + (bucket - 16) / 8;
  return (1ULL << exponent) + ((bucket - 16) % 8 + 1) * (1ULL << (exponent - 3)) - 1;
}
}

std::string_view pipeline_stage_name(PipelineStage stage) noexcept {
  static constexpr std::array<std::string_view, pipeline_stage_count> names{
      "scheduler_wake_lateness", "capture_acquire_cpu", "capture_copy_submit_cpu",
      "conversion_submit_cpu", "nvenc_submit_cpu", "nvenc_collect_cpu", "video_tick_cpu",
      "packet_fanout_cpu", "replay_append_cpu", "replay_snapshot_cpu",
      "writer_queue_residence", "writer_mux_cpu", "nvenc_submit_to_collection", "disk_write_cpu", "queue_admission_cpu"};
  const auto index = static_cast<std::size_t>(stage);
  return index < names.size() ? names[index] : "unknown";
}

void LatencyHistogram::record(std::uint64_t microseconds) noexcept {
  auto maximum = maximum_.load(std::memory_order_relaxed);
  while (maximum < microseconds && !maximum_.compare_exchange_weak(
      maximum, microseconds, std::memory_order_relaxed)) {}
  const auto bucket = microseconds > maximum_tracked_us ? bucket_count : bucket_for(microseconds);
  buckets_[bucket].fetch_add(1, std::memory_order_release);
}

LatencySummary LatencyHistogram::snapshot() const noexcept {
  std::array<std::uint64_t, bucket_count + 1> counts{};
  LatencySummary result;
  for (std::size_t i = 0; i < counts.size(); ++i) {
    counts[i] = buckets_[i].load(std::memory_order_acquire);
    result.count += counts[i];
  }
  result.max_us = maximum_.load(std::memory_order_relaxed);
  result.overflow_count = counts.back();
  if (!result.count) return {};
  const auto percentile = [&](std::uint64_t permille) {
    // ceil(count * permille / 1000), without multiplying the full count.
    const auto rank = result.count / 1000 * permille + (result.count % 1000 * permille + 999) / 1000;
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < counts.size(); ++i) {
      cumulative += counts[i];
      if (cumulative >= rank) return i == bucket_count ? result.max_us : upper_bound(i);
    }
    return result.max_us;
  };
  result.p50_us = percentile(500);
  result.p95_us = percentile(950);
  result.p99_us = percentile(990);
  result.p999_us = percentile(999);
  return result;
}

void LatencyHistogram::reset() noexcept {
  for (auto& bucket : buckets_) bucket.store(0, std::memory_order_relaxed);
  maximum_.store(0, std::memory_order_relaxed);
}

PipelineTelemetry::PipelineTelemetry() noexcept : session_id_(next_session.fetch_add(1, std::memory_order_relaxed)) {}
void PipelineTelemetry::configure(bool enabled, bool trace_enabled) noexcept {
  if (enabled && trace_enabled) { static TraceRegistration registration; }
  trace_enabled_.store(enabled && trace_enabled, std::memory_order_relaxed);
  enabled_.store(enabled, std::memory_order_relaxed);
}
void PipelineTelemetry::record(PipelineStage stage, std::uint64_t microseconds, std::uint64_t correlation) noexcept {
  const auto index = static_cast<std::size_t>(stage);
  if (!enabled() || index >= stages_.size()) return;
  stages_[index].record(microseconds);
  if (trace_enabled_.load(std::memory_order_relaxed) && TraceLoggingProviderEnabled(g_pipeline_provider, 0, 0)) {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    TraceLoggingWrite(g_pipeline_provider, "StageDuration",
        TraceLoggingString(pipeline_stage_name(stage).data(), "stage"),
        TraceLoggingUInt64(session_id_, "session_id"),
        TraceLoggingUInt32(static_cast<std::uint32_t>(correlation >> 32), "epoch_id"),
        TraceLoggingUInt32(static_cast<std::uint32_t>(correlation), "frame_id"),
        TraceLoggingInt64(qpc.QuadPart, "qpc"),
        TraceLoggingUInt64(microseconds, "observed_duration_us"));
  }
}
PipelineTelemetrySnapshot PipelineTelemetry::snapshot() const noexcept {
  PipelineTelemetrySnapshot result;
  result.enabled = enabled();
  result.trace_enabled = trace_enabled_.load(std::memory_order_relaxed);
  for (std::size_t i = 0; i < stages_.size(); ++i) result.stages[i] = stages_[i].snapshot();
  return result;
}
void PipelineTelemetry::reset() noexcept { for (auto& stage : stages_) stage.reset(); }

PipelineTimingScope::PipelineTimingScope(PipelineTelemetry& telemetry, PipelineStage stage, std::uint64_t correlation) noexcept
    : PipelineTimingScope(&telemetry, stage, correlation) {}
PipelineTimingScope::PipelineTimingScope(PipelineTelemetry* telemetry, PipelineStage stage, std::uint64_t correlation) noexcept
    : telemetry_(telemetry && telemetry->enabled() ? telemetry : nullptr), stage_(stage), correlation_(correlation) {
  if (telemetry_) start_ = std::chrono::steady_clock::now();
}
PipelineTimingScope::~PipelineTimingScope() {
  if (telemetry_) {
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count();
    telemetry_->record(stage_, duration > 0 ? static_cast<std::uint64_t>(duration) : 0, correlation_);
  }
}
}  // namespace rebelliocap
