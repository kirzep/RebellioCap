#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "cli/engine_metrics_json.h"

using namespace rebelliocap;

TEST_CASE("engine metrics JSON exposes save timing and continuity counters") {
  EngineMetrics metrics{};
  metrics.video_ticks = 100;
  metrics.missed_video_deadlines = 2;
  metrics.video_packets = 100;
  metrics.audio_packets = 300;
  metrics.audio_mixing_dropped_frames = 24'000;
  metrics.audio_unavailable_sources = 2;
  metrics.save_requests = 2;
  metrics.completed_saves = 1;
  metrics.failed_saves = 0;
  metrics.rejected_saves = 0;
  metrics.pipeline_errors = 0;
  metrics.continuous_packets = 77;
  metrics.continuous_failures = 1;
  metrics.continuous_recording_active = true;
  metrics.last_hotkey_save_latency_ticks = 25'000;
  metrics.replay_bytes = 64U * 1024U * 1024U;
  NvencRuntimeMetrics video{};
  video.submitted_frames = 99;
  video.completed_frames = 98;
  video.p95_latency_us = 1'500;

  const auto fields = engine_metrics_json(metrics, 10'000'000, video);

  REQUIRE(fields.at("video_packets") == 100);
  REQUIRE(fields.at("audio_packets") == 300);
  REQUIRE(fields.at("audio_mixing_dropped_frames") == 24'000);
  REQUIRE(fields.at("audio_unavailable_sources") == 2);
  REQUIRE(fields.at("save_requests") == 2);
  REQUIRE(fields.at("completed_saves") == 1);
  REQUIRE(fields.at("failed_saves") == 0);
  REQUIRE(fields.at("rejected_saves") == 0);
  REQUIRE(fields.at("pipeline_errors") == 0);
  REQUIRE(fields.at("continuous_packets") == 77);
  REQUIRE(fields.at("continuous_failures") == 1);
  REQUIRE(fields.at("continuous_recording_active") == true);
  REQUIRE(fields.at("hotkey_recognition_ms") == 2.5);
  REQUIRE(fields.at("replay_bytes") == 64U * 1024U * 1024U);
  REQUIRE(fields.at("encode_p95_us") == 1'500);
  REQUIRE(fields.at("submitted_frames") == 99);
  REQUIRE(fields.at("completed_frames") == 98);
  REQUIRE(fields.at("missed_video_deadlines") == 2);
  REQUIRE(std::abs(fields.at("dropped_frames_percent").get<double>() -
                   3.0 / 102.0 * 100.0) < 0.000001);
}
