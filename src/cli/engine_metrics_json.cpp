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
          {"hotkey_recognition_ms", hotkey_recognition_ms},
          {"submitted_frames", video_metrics.submitted_frames},
          {"completed_frames", video_metrics.completed_frames},
          {"encode_p50_us", video_metrics.p50_latency_us},
          {"encode_p95_us", video_metrics.p95_latency_us},
          {"dropped_frames_percent", dropped_frames_percent},
          {"qpc_frequency", qpc_frequency}};
}

}  // namespace rebelliocap
