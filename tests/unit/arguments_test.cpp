#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cli/arguments.h"

namespace {

using namespace rebelliocap;

Result<CliArguments> parse(std::initializer_list<std::wstring_view> values) {
  const std::vector<std::wstring_view> arguments(values);
  return parse_arguments(arguments);
}

LUID luid(std::uint64_t value) {
  return {.LowPart = static_cast<DWORD>(value),
          .HighPart = static_cast<LONG>(value >> 32U)};
}

}  // namespace

TEST_CASE("CLI exit codes remain stable for automation") {
  REQUIRE(static_cast<int>(CliExitCode::Success) == 0);
  REQUIRE(static_cast<int>(CliExitCode::InvalidArguments) == 2);
  REQUIRE(static_cast<int>(CliExitCode::UnsupportedHardware) == 3);
  REQUIRE(static_cast<int>(CliExitCode::CaptureFailure) == 4);
  REQUIRE(static_cast<int>(CliExitCode::AudioFailure) == 5);
  REQUIRE(static_cast<int>(CliExitCode::MuxFailure) == 6);
}

TEST_CASE("capture parses Unicode output and explicit monitor without losing defaults") {
  const auto result = parse({L"capture", L"--monitor", L"1311768467463790320:2",
                             L"--output", L"D:\\Клипы\\матч"});

  REQUIRE(result.is_success());
  const auto& arguments = result.value();
  REQUIRE(arguments.command == CliCommand::Capture);
  REQUIRE(arguments.monitor.has_value());
  REQUIRE(monitor_id_value(*arguments.monitor) == 0x123456789ABCDEF0ULL);
  REQUIRE(arguments.monitor->output_index == 2);
  REQUIRE(arguments.width == 1920);
  REQUIRE(arguments.height == 1080);
  REQUIRE(arguments.fps == 60);
  REQUIRE(arguments.hotkey.virtual_key == VK_F8);
  REQUIRE(arguments.output == L"D:\\Клипы\\матч");
}

TEST_CASE("option token cannot be consumed as a missing option value") {
  const auto result = parse({L"capture", L"--monitor", L"1:0", L"--output",
                             L"--fps", L"120"});

  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "cli.missing_option_value");
}

TEST_CASE("capture rejects invalid numeric ranges and unsupported containers") {
  const auto invalid_fps = parse({L"capture", L"--monitor", L"1:0", L"--output",
                                  L"clip", L"--fps", L"0"});
  const auto invalid_duration = parse({L"capture", L"--monitor", L"1:0", L"--output",
                                       L"clip", L"--buffer-seconds", L"5",
                                       L"--clip-seconds", L"6"});
  const auto invalid_container = parse({L"smoke", L"--output", L"clip.mov",
                                        L"--container", L"mov"});

  REQUIRE_FALSE(invalid_fps.is_success());
  REQUIRE(invalid_fps.error().code == "cli.invalid_option_value");
  REQUIRE_FALSE(invalid_duration.is_success());
  REQUIRE(invalid_duration.error().code == "cli.invalid_option_value");
  REQUIRE_FALSE(invalid_container.is_success());
  REQUIRE(invalid_container.error().code == "cli.invalid_option_value");
}

TEST_CASE("monitor selection rejects identifiers absent from current DXGI catalog") {
  const std::vector<MonitorInfo> monitors{{.id = {luid(7), 0}, .name = L"DISPLAY1"},
                                           {.id = {luid(9), 1}, .name = L"DISPLAY2"}};

  const auto result = select_monitor({luid(8), 0}, monitors);

  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "cli.unknown_monitor");
}

TEST_CASE("smoke command requires an output and accepts an automatic duration") {
  const auto missing_output = parse({L"smoke", L"--seconds", L"30"});
  const auto parsed = parse({L"smoke", L"--seconds", L"45", L"--container", L"mkv",
                             L"--output", L"D:\\Клипы\\smoke.mkv"});

  REQUIRE_FALSE(missing_output.is_success());
  REQUIRE(missing_output.error().code == "cli.missing_required_option");
  REQUIRE(parsed.is_success());
  REQUIRE(parsed.value().command == CliCommand::Smoke);
  REQUIRE(parsed.value().seconds == 45);
  REQUIRE(parsed.value().container == Container::Mkv);
}

TEST_CASE("capture accepts a bounded automatic stop duration for evidence runs") {
  const auto parsed = parse({L"capture", L"--monitor", L"1:0", L"--output", L"clips",
                             L"--duration-seconds", L"1800"});
  const auto invalid = parse({L"capture", L"--monitor", L"1:0", L"--output", L"clips",
                              L"--duration-seconds", L"0"});

  REQUIRE(parsed.is_success());
  REQUIRE(parsed.value().duration_seconds.has_value());
  REQUIRE(*parsed.value().duration_seconds == 1800);
  REQUIRE_FALSE(invalid.is_success());
  REQUIRE(invalid.error().code == "cli.invalid_option_value");
}

TEST_CASE("audio endpoint catalog command accepts no session options") {
  const auto parsed = parse({L"list-audio-devices"});
  const auto with_option = parse({L"list-audio-devices", L"--output", L"clips"});

  REQUIRE(parsed.is_success());
  REQUIRE(parsed.value().command == CliCommand::ListAudioDevices);
  REQUIRE_FALSE(with_option.is_success());
  REQUIRE(with_option.error().code == "cli.unknown_option");
}

TEST_CASE("session parses every startup option without normalizing endpoint or output text") {
  const auto parsed = parse({
      L"session", L"--monitor", L"60081:0", L"--microphone-id",
      L"{0.0.1.00000000}.{Микрофон-Ä}", L"--width", L"1920", L"--height", L"1080",
      L"--fps", L"60", L"--bitrate", L"30000000", L"--container", L"mp4",
      L"--replay-enabled", L"true", L"--buffer-seconds", L"30", L"--clip-seconds",
      L"30", L"--replay-hotkey", L"Alt+F10", L"--recording-hotkey",
      L"Ctrl+Shift+R", L"--output", L"C:\\Users\\User\\Videos\\RebellioCap"});

  REQUIRE(parsed.is_success());
  const auto& arguments = parsed.value();
  REQUIRE(arguments.command == CliCommand::Session);
  REQUIRE(arguments.monitor.has_value());
  REQUIRE(monitor_id_value(*arguments.monitor) == 60081);
  REQUIRE(arguments.monitor->output_index == 0);
  REQUIRE(arguments.microphone_id == L"{0.0.1.00000000}.{Микрофон-Ä}");
  REQUIRE(arguments.width == 1920);
  REQUIRE(arguments.height == 1080);
  REQUIRE(arguments.fps == 60);
  REQUIRE(arguments.bitrate == 30'000'000);
  REQUIRE(arguments.container == Container::Mp4);
  REQUIRE(arguments.replay_enabled);
  REQUIRE(arguments.buffer_seconds == 30);
  REQUIRE(arguments.clip_seconds == 30);
  REQUIRE(arguments.replay_hotkey ==
          HotkeyChord{.virtual_key = VK_F10, .alt = true});
  REQUIRE(arguments.recording_hotkey ==
          HotkeyChord{.virtual_key = L'R', .control = true, .shift = true});
  REQUIRE(arguments.output == L"C:\\Users\\User\\Videos\\RebellioCap");
}

TEST_CASE("hidden native modes accept only a host configuration file") {
  REQUIRE(parse({L"doctor-json"}).is_success());
  REQUIRE(parse({L"catalog-json"}).is_success());
  REQUIRE(parse({L"session-json", L"--config", L"C:\\draft.json"}).is_success());
  REQUIRE(parse({L"test-json", L"--config", L"C:\\draft.json"}).is_success());
  REQUIRE_FALSE(parse({L"session-json"}).is_success());
  REQUIRE_FALSE(parse({L"test-json", L"--seconds", L"30"}).is_success());
}

TEST_CASE("doctor accepts an explicit monitor without capture options") {
  for (const auto command : {L"doctor", L"doctor-json"}) {
    const auto result = parse({command, L"--monitor", L"42:1"});
    REQUIRE(result.is_success());
    REQUIRE(result.value().monitor.has_value());
    REQUIRE(monitor_id_text(*result.value().monitor) == "42:1");
    REQUIRE_FALSE(parse({command, L"--monitor", L"invalid"}).is_success());
    REQUIRE_FALSE(parse({command, L"--fps", L"60"}).is_success());
  }
}

TEST_CASE("diagnostics select the requested adapter and never fall back for a missing monitor") {
  const std::vector<MonitorInfo> monitors{
      {.id = {.adapter_luid = luid(1), .output_index = 0}, .primary = true},
      {.id = {.adapter_luid = luid(42), .output_index = 1}, .primary = false}};
  const auto selected = select_diagnostic_monitor(MonitorId{luid(42), 1}, monitors);
  REQUIRE(selected.is_success());
  REQUIRE(monitor_id_text(selected.value().id) == "42:1");
  REQUIRE(monitor_id_text(select_diagnostic_monitor(std::nullopt, monitors).value().id) == "1:0");
  REQUIRE_FALSE(select_diagnostic_monitor(MonitorId{luid(99), 0}, monitors).is_success());
  REQUIRE_FALSE(select_diagnostic_monitor(std::nullopt, {}).is_success());
  const std::vector<MonitorInfo> no_primary{
      {.id = {.adapter_luid = luid(42), .output_index = 1}, .primary = false}};
  REQUIRE(monitor_id_text(select_diagnostic_monitor(std::nullopt, no_primary).value().id) == "42:1");
}

TEST_CASE("session requires monitor microphone and output before hardware startup") {
  const auto missing_monitor =
      parse({L"session", L"--microphone-id", L"endpoint", L"--output", L"clips"});
  const auto missing_microphone =
      parse({L"session", L"--monitor", L"1:0", L"--output", L"clips"});
  const auto missing_output = parse(
      {L"session", L"--monitor", L"1:0", L"--microphone-id", L"endpoint"});

  REQUIRE_FALSE(missing_monitor.is_success());
  REQUIRE(missing_monitor.error().code == "cli.missing_required_option");
  REQUIRE_FALSE(missing_microphone.is_success());
  REQUIRE(missing_microphone.error().code == "cli.missing_required_option");
  REQUIRE_FALSE(missing_output.is_success());
  REQUIRE(missing_output.error().code == "cli.missing_required_option");
}

TEST_CASE("session rejects replay duration beyond its capacity") {
  const auto parsed = parse({L"session", L"--monitor", L"1:0", L"--microphone-id",
                             L"endpoint", L"--buffer-seconds", L"5", L"--clip-seconds",
                             L"6", L"--output", L"clips"});

  REQUIRE_FALSE(parsed.is_success());
  REQUIRE(parsed.error().code == "cli.invalid_option_value");
}

TEST_CASE("session replay flag accepts only literal lowercase booleans") {
  const auto disabled = parse({L"session", L"--monitor", L"1:0", L"--microphone-id",
                               L"endpoint", L"--replay-enabled", L"false", L"--output",
                               L"clips"});
  const auto invalid = parse({L"session", L"--monitor", L"1:0", L"--microphone-id",
                              L"endpoint", L"--replay-enabled", L"True", L"--output",
                              L"clips"});

  REQUIRE(disabled.is_success());
  REQUIRE_FALSE(disabled.value().replay_enabled);
  REQUIRE_FALSE(invalid.is_success());
  REQUIRE(invalid.error().code == "cli.invalid_option_value");
}

TEST_CASE("session rejects distinct text that normalizes to one hotkey chord") {
  const auto parsed = parse({L"session", L"--monitor", L"1:0", L"--microphone-id",
                             L"endpoint", L"--replay-hotkey", L"shift+ctrl+r",
                             L"--recording-hotkey", L"Ctrl+Shift+R", L"--output", L"clips"});

  REQUIRE_FALSE(parsed.is_success());
  REQUIRE(parsed.error().code == "hotkey.duplicate_chord");
}

TEST_CASE("session preserves hotkey validation errors") {
  const auto parsed = parse({L"session", L"--monitor", L"1:0", L"--microphone-id",
                             L"endpoint", L"--replay-hotkey", L"Alt+F4", L"--output",
                             L"clips"});

  REQUIRE_FALSE(parsed.is_success());
  REQUIRE(parsed.error().code == "hotkey.reserved_chord");
}

TEST_CASE("session option token cannot be consumed as a missing option value") {
  const auto parsed = parse({L"session", L"--monitor", L"1:0", L"--microphone-id",
                             L"--output", L"clips"});

  REQUIRE_FALSE(parsed.is_success());
  REQUIRE(parsed.error().code == "cli.missing_option_value");
}

TEST_CASE("session numeric bounds match the capture contract") {
  const std::vector<std::pair<std::wstring_view, std::wstring_view>> invalid_options{
      {L"--width", L"63"},          {L"--width", L"16385"},
      {L"--height", L"63"},         {L"--height", L"16385"},
      {L"--fps", L"0"},             {L"--fps", L"241"},
      {L"--bitrate", L"99999"},     {L"--bitrate", L"500000001"},
      {L"--buffer-seconds", L"0"},  {L"--clip-seconds", L"0"}};

  for (const auto& [option, value] : invalid_options) {
    CAPTURE(option, value);
    const std::vector<std::wstring_view> values{
        L"session", L"--monitor", L"1:0", L"--microphone-id", L"endpoint",
        option, value, L"--output", L"clips"};
    const auto parsed = parse_arguments(values);
    REQUIRE_FALSE(parsed.is_success());
    REQUIRE(parsed.error().code == "cli.invalid_option_value");
  }
}
