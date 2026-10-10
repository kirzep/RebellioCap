#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "capture/monitor_catalog.h"
#include "config/recording_contract.h"
#include "hotkey/hotkey_chord.h"
#include "mux/clip_muxer.h"

namespace rebelliocap {

struct EngineConfig {
  MonitorId monitor{};
  std::wstring system_audio_id;
  std::wstring microphone_id;
  bool microphone_enabled{true};
  bool system_audio_enabled{true};
  std::uint32_t width{1920};
  std::uint32_t height{1080};
  std::uint32_t fps{60};
  std::uint32_t bitrate{30'000'000};
  std::uint32_t gop_seconds{2};
  std::chrono::seconds replay_capacity{30};
  std::chrono::seconds clip_duration{30};
  Container container{Container::Mp4};
  std::filesystem::path output_directory;
  bool save_without_game_folders{false};
  HotkeyChord save_replay_hotkey{recording_default_hotkey("save_replay_hotkey")};
  HotkeyChord toggle_recording_hotkey{recording_default_hotkey("toggle_recording_hotkey")};
  bool continuous_recording_enabled{false};
  std::size_t maximum_pending_saves{16};
  // User budget in MiB; zero preserves the automatic RAM policy.
  std::uint32_t replay_memory_limit_mb{0};
  // Internal hard cap for deterministic tests and embedding.
  std::size_t maximum_buffer_bytes{0};
  std::filesystem::path naming_settings_file;
};

struct EngineMetrics {
  std::uint64_t video_ticks{0};
  std::uint64_t missed_video_deadlines{0};
  std::uint64_t video_packets{0};
  std::uint64_t audio_packets{0};
  std::uint64_t audio_mixing_dropped_frames{0};
  std::uint32_t audio_unavailable_sources{0};
  std::uint64_t save_requests{0};
  std::uint64_t completed_saves{0};
  std::uint64_t failed_saves{0};
  std::uint64_t rejected_saves{0};
  std::uint64_t pipeline_errors{0};
  std::uint64_t continuous_packets{0};
  std::uint64_t continuous_failures{0};
  bool continuous_recording_active{false};
  QpcTicks last_hotkey_save_latency_ticks{0};
  std::size_t replay_bytes{0};
  std::size_t budget_bytes{0};
  std::size_t buffered_bytes{0};
  double replay_retained_seconds{0};
  bool replay_memory_limited{false};
  std::uint64_t replay_memory_drops{0};
  std::string continuous_recovery_path;
};

}  // namespace rebelliocap
