#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "capture/monitor_catalog.h"
#include "core/result.h"
#include "hotkey/hotkey_chord.h"
#include "mux/clip_muxer.h"

namespace rebelliocap {

enum class CliCommand { Doctor, ListMonitors, ListAudioDevices, Capture, Smoke, Session,
  DoctorJson, CatalogJson, TestJson, SessionJson };

enum class CliExitCode : int {
  Success = 0,
  InvalidArguments = 2,
  UnsupportedHardware = 3,
  CaptureFailure = 4,
  AudioFailure = 5,
  MuxFailure = 6,
};

struct CliArguments {
  CliCommand command{CliCommand::Doctor};
  std::optional<MonitorId> monitor;
  std::wstring microphone_id;
  std::uint32_t width{1920};
  std::uint32_t height{1080};
  std::uint32_t fps{60};
  std::uint32_t bitrate{30'000'000};
  std::uint32_t buffer_seconds{30};
  std::uint32_t clip_seconds{30};
  std::uint32_t seconds{30};
  std::optional<std::uint32_t> duration_seconds;
  Container container{Container::Mp4};
  bool replay_enabled{true};
  std::filesystem::path output;
  std::filesystem::path config_path;
  HotkeyChord hotkey{VK_F8, false, false, false};
  HotkeyChord replay_hotkey{VK_F8, false, false, false};
  HotkeyChord recording_hotkey{static_cast<std::uint16_t>(L'R'), true, false, true};
};

Result<CliArguments> parse_arguments(std::span<const std::wstring_view> arguments);
Result<MonitorInfo> select_monitor(MonitorId requested,
                                   std::span<const MonitorInfo> monitors);
Result<MonitorInfo> select_diagnostic_monitor(std::optional<MonitorId> requested,
                                            std::span<const MonitorInfo> monitors);
[[nodiscard]] std::uint64_t monitor_id_value(MonitorId id) noexcept;
[[nodiscard]] std::string monitor_id_text(MonitorId id);

}  // namespace rebelliocap
