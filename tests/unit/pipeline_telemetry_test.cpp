#include <array>
#include <limits>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "core/pipeline_telemetry.h"

using namespace rebelliocap;

TEST_CASE("telemetry empty and disabled paths do not invent samples") {
  PipelineTelemetry telemetry;
  { PipelineTimingScope scope(telemetry, PipelineStage::VideoTickCpu); }
  telemetry.record(PipelineStage::VideoTickCpu, 123);
  const auto sample = telemetry.snapshot().stages[static_cast<std::size_t>(PipelineStage::VideoTickCpu)];
  REQUIRE(sample.count == 0);
  REQUIRE(sample.max_us == 0);
  REQUIRE(sample.p999_us == 0);
}

TEST_CASE("latency bins bound values at powers and subdivision edges") {
  for (const auto [value, upper] : std::array<std::array<std::uint64_t, 2>, 9>{
       {{0,0}, {1,1}, {7,7}, {8,8}, {15,15}, {16,17}, {17,17}, {18,19}, {31,31}}}) {
    LatencyHistogram histogram;
    histogram.record(value);
    const auto sample = histogram.snapshot();
    REQUIRE(sample.count == 1);
    REQUIRE(sample.p50_us == upper);
    REQUIRE(sample.p999_us == upper);
    REQUIRE(sample.max_us == value);
    REQUIRE(sample.overflow_count == 0);
  }
}

TEST_CASE("latency histogram uses nearest rank for tail percentiles") {
  LatencyHistogram histogram;
  for (int i = 0; i < 1000; ++i) histogram.record(i < 500 ? 1 : i < 950 ? 2 : i < 990 ? 3 : i < 999 ? 4 : 5);
  const auto sample = histogram.snapshot();
  REQUIRE(sample.count == 1000);
  REQUIRE(sample.p50_us == 1);
  REQUIRE(sample.p95_us == 2);
  REQUIRE(sample.p99_us == 3);
  REQUIRE(sample.p999_us == 4);
  REQUIRE(sample.max_us == 5);
}

TEST_CASE("latency overflow preserves count maximum and tail instead of clamping") {
  LatencyHistogram histogram;
  histogram.record(LatencyHistogram::maximum_tracked_us);
  histogram.record(LatencyHistogram::maximum_tracked_us + 1);
  histogram.record((std::numeric_limits<std::uint64_t>::max)());
  const auto sample = histogram.snapshot();
  REQUIRE(sample.count == 3);
  REQUIRE(sample.overflow_count == 2);
  REQUIRE(sample.p50_us == (std::numeric_limits<std::uint64_t>::max)());
  REQUIRE(sample.max_us == (std::numeric_limits<std::uint64_t>::max)());
}

TEST_CASE("telemetry retains accumulated samples across idle and resumes only when enabled") {
  PipelineTelemetry telemetry;
  telemetry.configure(true);
  telemetry.record(PipelineStage::SchedulerWakeLateness, 4);
  telemetry.configure(false);
  telemetry.record(PipelineStage::SchedulerWakeLateness, 99);
  telemetry.configure(true);
  telemetry.record(PipelineStage::SchedulerWakeLateness, 6);
  auto sample = telemetry.snapshot().stages[0];
  REQUIRE(sample.count == 2);
  REQUIRE(sample.max_us == 6);
  telemetry.reset();
  REQUIRE(telemetry.snapshot().stages[0].count == 0);
}

TEST_CASE("timing scopes record only enabled scopes and tolerate absent telemetry") {
  PipelineTelemetry telemetry;
  { PipelineTimingScope scope(nullptr, PipelineStage::VideoTickCpu); }
  telemetry.configure(true);
  { PipelineTimingScope scope(telemetry, PipelineStage::VideoTickCpu, (2ULL << 32) | 7); }
  const auto sample = telemetry.snapshot().stages[static_cast<std::size_t>(PipelineStage::VideoTickCpu)];
  REQUIRE(sample.count == 1);
  REQUIRE(telemetry.snapshot().trace_enabled == false);
}

TEST_CASE("latency concurrent readers and producers converge without losing samples") {
  LatencyHistogram histogram;
  std::array<std::jthread, 3> producers;
  for (auto& producer : producers) producer = std::jthread([&] { for (int i=0;i<10000;++i) histogram.record(42); });
  for (int i=0;i<100;++i) { const auto sample = histogram.snapshot(); REQUIRE(sample.count <= 30000); }
  for (auto& producer : producers) producer.join();
  REQUIRE(histogram.snapshot().count == 30000);
  REQUIRE(histogram.snapshot().max_us == 42);
}
