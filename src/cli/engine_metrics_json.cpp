#include "cli/engine_metrics_json.h"

namespace rebelliocap {

nlohmann::json engine_metrics_json(const EngineMetrics& metrics,
                                   QpcTicks qpc_frequency,
                                   const NvencRuntimeMetrics& video_metrics) {
  const double hotkey_recognition_ms = qpc_frequency > 0
      ? static_cast<double>(metrics.last_hotkey_save_latency_ticks) * 1000.0 /
            static_cast<double>(qpc_frequency)
      : 0.0;
  const auto rejected_frames = metrics.video_ticks > video_metrics.submitted_frames
      ? metrics.video_ticks - video_metrics.submitted_frames
      : 0;
  const auto dropped_frames = metrics.missed_video_deadlines + rejected_frames;
  const auto expected_frames = metrics.video_ticks + metrics.missed_video_deadlines;
  const double dropped_frames_percent = expected_frames > 0
      ? static_cast<double>(dropped_frames) * 100.0 /
            static_cast<double>(expected_frames)
      : 0.0;
  nlohmann::json stages = nlohmann::json::object();
  for (std::size_t i = 0; i < pipeline_stage_count; ++i) {
    const auto& timing = metrics.pipeline_telemetry.stages[i];
    stages[std::string(pipeline_stage_name(static_cast<PipelineStage>(i)))] = {
        {"count", timing.count}, {"p50_us", timing.p50_us},
        {"p95_us", timing.p95_us}, {"p99_us", timing.p99_us},
        {"p999_us", timing.p999_us}, {"max_us", timing.max_us},
        {"overflow_count", timing.overflow_count}};
  }
  const auto& queue = metrics.continuous_queue;
  const auto& capture=video_metrics.capture_session;
  const auto value_or_null=[&](auto value){return capture.available?nlohmann::json(value):nlohmann::json(nullptr);};
  nlohmann::json capture_fields={{"available",capture.available},
      {"api",value_or_null(capture.duplicate_output1?"DuplicateOutput1":"DuplicateOutput")},
      {"adapter_luid_low",value_or_null(capture.adapter_luid_low)},
      {"adapter_luid_high",value_or_null(capture.adapter_luid_high)},
      {"texture_format",capture.available&&capture.texture_format_available?nlohmann::json(capture.texture_format):nlohmann::json(nullptr)},
      {"fallback_hresult",capture.available&&capture.fallback_hresult?nlohmann::json(*capture.fallback_hresult):nlohmann::json(nullptr)}};
  const double queue_age_us = qpc_frequency > 0 && queue.oldest_age_ticks > 0
      ? static_cast<double>(queue.oldest_age_ticks) * 1'000'000.0 / static_cast<double>(qpc_frequency)
      : 0.0;
  return {{"video_ticks", metrics.video_ticks},
          {"missed_video_deadlines", metrics.missed_video_deadlines},
          {"video_packets", metrics.video_packets},
          {"audio_packets", metrics.audio_packets},
          {"audio_mixing_dropped_frames", metrics.audio_mixing_dropped_frames},
          {"audio_unavailable_sources", metrics.audio_unavailable_sources},
          {"replay_bytes", metrics.replay_bytes},
          {"budget_bytes", metrics.budget_bytes},
          {"buffered_bytes", metrics.buffered_bytes},
          {"replay_retained_seconds", metrics.replay_retained_seconds},
          {"replay_memory_limited", metrics.replay_memory_limited},
          {"replay_memory_drops", metrics.replay_memory_drops},
          {"save_requests", metrics.save_requests},
          {"completed_saves", metrics.completed_saves},
          {"failed_saves", metrics.failed_saves},
          {"rejected_saves", metrics.rejected_saves},
          {"pipeline_errors", metrics.pipeline_errors},
          {"continuous_packets", metrics.continuous_packets},
          {"continuous_failures", metrics.continuous_failures},
          {"continuous_recovery_path", metrics.continuous_recovery_path},
          {"continuous_recording_active", metrics.continuous_recording_active},
          {"continuous_queue", {{"queued_bytes", queue.queued_bytes},
              {"in_flight_bytes", queue.in_flight_bytes},
              {"queued_packets", queue.queued_packets},
              {"in_flight_packets", queue.in_flight_packets},
              {"oldest_enqueue_qpc", queue.oldest_enqueue_qpc},
              {"oldest_age_us", queue_age_us},
              {"high_water_bytes", queue.high_water_bytes},
              {"warning", queue.warning},
              {"warning_transitions", queue.warning_transitions}}},
          {"hotkey_recognition_ms", hotkey_recognition_ms},
          {"submitted_frames", video_metrics.submitted_frames},
          {"completed_frames", video_metrics.completed_frames},
          {"backpressure_dropped_frames", video_metrics.backpressure_dropped_frames},
          {"oldest_in_flight_age_us", video_metrics.oldest_in_flight_age_us},
          {"nvenc_in_flight_frames", video_metrics.in_flight_frames},
          {"nvenc_slot_capacity", video_metrics.slot_capacity},
          {"new_image_ticks", video_metrics.new_image_ticks},
          {"repeated_image_ticks", video_metrics.repeated_image_ticks},
          {"no_image_ticks", video_metrics.no_image_ticks},
          {"converter_cache_hits", video_metrics.converter_cache_hits},
          {"converter_pool_capacity", video_metrics.converter_pool_capacity},
          {"converter_input_views_created", video_metrics.converter_input_views_created},
          {"capture_cache_backend", video_metrics.direct_nv12_cache ? "direct_nv12" : "bgra"},
          {"capture_session",std::move(capture_fields)},
          {"nvenc_accepted_config", {{"available",video_metrics.accepted_config.width != 0},
              {"preset",video_metrics.accepted_config.preset_number},
              {"multipass",video_metrics.accepted_config.multipass},
              {"b_frames",video_metrics.accepted_config.frame_interval_p ? video_metrics.accepted_config.frame_interval_p-1 : 0},
              {"rate_control",video_metrics.accepted_config.rate_control},
              {"target_quality",video_metrics.accepted_config.target_quality},
              {"target_bitrate",video_metrics.accepted_config.target_bitrate},
              {"max_bitrate",video_metrics.accepted_config.max_bitrate},
              {"gop_frames",video_metrics.accepted_config.gop_frames}}},
          {"nvenc_max_in_flight_frames", video_metrics.max_in_flight_frames},
          {"encode_p50_us", video_metrics.p50_latency_us},
          {"encode_p95_us", video_metrics.p95_latency_us},
          // Existing encoder latency includes driver/B-frame buffering and CPU
          // polling delay. It does not measure GPU execution time.
          {"encode_latency_semantics", "cpu_submit_to_bitstream_collection"},
          {"gpu_execution_timing_available", false},
          {"pipeline_telemetry", {{"enabled", metrics.pipeline_telemetry.enabled},
              {"trace_enabled", metrics.pipeline_telemetry.trace_enabled},
              {"unit", "microseconds"},
              {"percentile_semantics", "bin_upper_bound_overflow_uses_observed_max"},
              {"maximum_tracked_us", LatencyHistogram::maximum_tracked_us},
              {"concurrent_snapshot", "approximate_atomic_bins"},
              {"stages", std::move(stages)}}},
          {"dropped_frames_percent", dropped_frames_percent},
          {"qpc_frequency", qpc_frequency}};
}

}  // namespace rebelliocap
