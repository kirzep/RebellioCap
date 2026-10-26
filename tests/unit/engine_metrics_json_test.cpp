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
  video.direct_nv12_cache = true;
  video.converter_input_views_created = 2;
  video.p95_latency_us = 1'500;
  video.backpressure_dropped_frames = 1;
  video.oldest_in_flight_age_us = 12'000'000;

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
  REQUIRE(fields.at("capture_cache_backend") == "direct_nv12");
  REQUIRE(fields.at("converter_input_views_created") == 2);
  REQUIRE(fields.at("backpressure_dropped_frames") == 1);
  REQUIRE(fields.at("oldest_in_flight_age_us") == 12'000'000);
  REQUIRE(fields.at("missed_video_deadlines") == 2);
  REQUIRE(std::abs(fields.at("dropped_frames_percent").get<double>() -
                   3.0 / 102.0 * 100.0) < 0.000001);
}

TEST_CASE("metrics explain observed encode latency and do not double count backpressure") {
  EngineMetrics metrics{};
  metrics.video_ticks = 100;
  metrics.missed_video_deadlines = 5;
  NvencRuntimeMetrics video{};
  video.submitted_frames = 90;
  video.backpressure_dropped_frames = 10;
  const auto fields = engine_metrics_json(metrics, 0, video);
  REQUIRE(std::abs(fields.at("dropped_frames_percent").get<double>() - 15.0 / 105.0 * 100.0) < 0.000001);
  REQUIRE(fields.at("encode_latency_semantics") == "cpu_submit_to_bitstream_collection");
  REQUIRE(fields.at("gpu_execution_timing_available") == false);
  REQUIRE(fields.at("pipeline_telemetry").at("enabled") == false);
  REQUIRE(fields.at("pipeline_telemetry").at("stages").at("video_tick_cpu").at("count") == 0);
}

TEST_CASE("metrics publish stage percentiles units overflow and NVENC occupancy") {
  EngineMetrics metrics{};
  metrics.pipeline_telemetry.enabled = true;
  metrics.pipeline_telemetry.stages[static_cast<std::size_t>(PipelineStage::VideoTickCpu)] =
      {.count=1000, .p50_us=1, .p95_us=2, .p99_us=3, .p999_us=4, .max_us=9, .overflow_count=1};
  NvencRuntimeMetrics video{};
  video.slot_capacity = 6;
  video.max_in_flight_frames = 5;
  const auto fields = engine_metrics_json(metrics, 10'000'000, video);
  const auto& telemetry = fields.at("pipeline_telemetry");
  REQUIRE(telemetry.at("enabled") == true);
  REQUIRE(telemetry.at("unit") == "microseconds");
  REQUIRE(telemetry.at("percentile_semantics") == "bin_upper_bound_overflow_uses_observed_max");
  const auto& timing = telemetry.at("stages").at("video_tick_cpu");
  REQUIRE(timing.at("count") == 1000);
  REQUIRE(timing.at("p50_us") == 1);
  REQUIRE(timing.at("p95_us") == 2);
  REQUIRE(timing.at("p99_us") == 3);
  REQUIRE(timing.at("p999_us") == 4);
  REQUIRE(timing.at("max_us") == 9);
  REQUIRE(timing.at("overflow_count") == 1);
  REQUIRE(fields.at("nvenc_slot_capacity") == 6);
  REQUIRE(fields.at("nvenc_max_in_flight_frames") == 5);
}

TEST_CASE("metrics publish actual accepted encoder configuration without assuming preset multipass") {
  EngineMetrics metrics{}; NvencRuntimeMetrics video{};
  video.accepted_config.width=1920; video.accepted_config.preset_number=4;
  video.accepted_config.multipass=2; video.accepted_config.frame_interval_p=2;
  const auto fields=engine_metrics_json(metrics,10'000'000,video);
  REQUIRE(fields.at("nvenc_accepted_config").at("available")==true);
  REQUIRE(fields.at("nvenc_accepted_config").at("preset")==4);
  REQUIRE(fields.at("nvenc_accepted_config").at("multipass")==2);
  REQUIRE(fields.at("nvenc_accepted_config").at("b_frames")==1);
}

TEST_CASE("capture diagnostics distinguish an unavailable session from legacy BGRA capture") {
  const auto fields=engine_metrics_json(EngineMetrics{},10'000'000,NvencRuntimeMetrics{});
  REQUIRE(fields.contains("capture_session"));
  const auto& capture=fields.at("capture_session");
  CHECK(capture.at("available")==false);
  CHECK(capture.at("api").is_null());
  CHECK(capture.at("texture_format").is_null());
  CHECK(capture.at("adapter_luid_low").is_null());
  CHECK(capture.at("adapter_luid_high").is_null());
  CHECK(capture.at("fallback_hresult").is_null());
}

TEST_CASE("capture diagnostics publish actual formats LUID and modern fallback without guessing") {
  NvencRuntimeMetrics video{};
  video.capture_session.available=true;
  video.capture_session.adapter_luid_low=0xfedcba98U;
  video.capture_session.adapter_luid_high=-2;
  auto capture=engine_metrics_json(EngineMetrics{},10'000'000,video).at("capture_session");
  CHECK(capture.at("api")=="DuplicateOutput");
  CHECK(capture.at("texture_format").is_null()); // No texture has arrived yet.
  video.capture_session.texture_format_available=true;
  video.capture_session.texture_format=87;
  video.capture_session.duplicate_output1=true;
  capture=engine_metrics_json(EngineMetrics{},10'000'000,video).at("capture_session");
  CHECK(capture.at("api")=="DuplicateOutput1");
  CHECK(capture.at("texture_format")==87);
  CHECK(capture.at("adapter_luid_low")==0xfedcba98U);
  CHECK(capture.at("adapter_luid_high")==-2);
  CHECK(capture.at("fallback_hresult").is_null());
  video.capture_session.duplicate_output1=false;
  video.capture_session.fallback_hresult=static_cast<long>(E_NOINTERFACE);
  capture=engine_metrics_json(EngineMetrics{},10'000'000,video).at("capture_session");
  CHECK(capture.at("api")=="DuplicateOutput");
  CHECK(capture.at("fallback_hresult")==static_cast<long>(E_NOINTERFACE));
  video.capture_session.available=false;
  capture=engine_metrics_json(EngineMetrics{},10'000'000,video).at("capture_session");
  CHECK(capture.at("api").is_null());
  CHECK(capture.at("texture_format").is_null());
  CHECK(capture.at("adapter_luid_low").is_null());
  CHECK(capture.at("fallback_hresult").is_null());
}
