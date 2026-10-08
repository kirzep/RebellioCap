#include "cli/arguments.h"

#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace rebelliocap {
namespace {

Error cli_error(std::string code, std::string message) {
  return {.code = std::move(code), .message = std::move(message), .hresult = std::nullopt};
}

bool is_option(std::wstring_view value) { return value.starts_with(L"--"); }

Result<std::uint64_t> decimal(std::wstring_view text) {
  if (text.empty()) {
    return Result<std::uint64_t>::failure(
        cli_error("cli.invalid_option_value", "A numeric option value is empty."));
  }
  std::uint64_t value = 0;
  for (const wchar_t character : text) {
    if (character < L'0' || character > L'9') {
      return Result<std::uint64_t>::failure(
          cli_error("cli.invalid_option_value", "A numeric option value is not decimal."));
    }
    const auto digit = static_cast<std::uint64_t>(character - L'0');
    if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10U) {
      return Result<std::uint64_t>::failure(
          cli_error("cli.invalid_option_value", "A numeric option value is too large."));
    }
    value = value * 10U + digit;
  }
  return Result<std::uint64_t>::success(value);
}

Result<std::uint32_t> bounded_decimal(std::wstring_view text, std::uint32_t minimum,
                                      std::uint32_t maximum) {
  auto parsed = decimal(text);
  if (!parsed.is_success() || parsed.value() < minimum || parsed.value() > maximum) {
    return Result<std::uint32_t>::failure(
        cli_error("cli.invalid_option_value", "A numeric option is outside its supported range."));
  }
  return Result<std::uint32_t>::success(static_cast<std::uint32_t>(parsed.value()));
}

Result<MonitorId> parse_monitor(std::wstring_view text) {
  const auto separator = text.find(L':');
  if (separator == std::wstring_view::npos || separator == 0 ||
      separator + 1 >= text.size() || text.find(L':', separator + 1) != std::wstring_view::npos) {
    return Result<MonitorId>::failure(
        cli_error("cli.invalid_option_value", "Monitor must use the <luid>:<output-index> form."));
  }
  auto adapter = decimal(text.substr(0, separator));
  auto output = bounded_decimal(text.substr(separator + 1), 0,
                                (std::numeric_limits<std::uint32_t>::max)());
  if (!adapter.is_success() || !output.is_success()) {
    return Result<MonitorId>::failure(
        cli_error("cli.invalid_option_value", "Monitor contains an invalid LUID or output index."));
  }
  const auto value = adapter.value();
  return Result<MonitorId>::success({
      .adapter_luid = {.LowPart = static_cast<DWORD>(value),
                       .HighPart = static_cast<LONG>(value >> 32U)},
      .output_index = output.value()});
}

Result<HotkeyChord> parse_hotkey(std::wstring_view text) {
  if (text.size() >= 2 && text[0] == L'F') {
    auto number = bounded_decimal(text.substr(1), 1, 24);
    if (number.is_success()) {
      return Result<HotkeyChord>::success(
          {.virtual_key = static_cast<std::uint16_t>(VK_F1 + number.value() - 1)});
    }
  }
  if (text.size() == 1 && ((text[0] >= L'A' && text[0] <= L'Z') ||
                           (text[0] >= L'0' && text[0] <= L'9'))) {
    return Result<HotkeyChord>::success(
        {.virtual_key = static_cast<std::uint16_t>(text[0])});
  }
  return Result<HotkeyChord>::failure(
      cli_error("cli.invalid_option_value", "Hotkey must be F1-F24, A-Z, or 0-9."));
}

Result<std::wstring_view> option_value(std::span<const std::wstring_view> arguments,
                                       std::size_t& index) {
  if (index + 1 >= arguments.size() || is_option(arguments[index + 1])) {
    return Result<std::wstring_view>::failure(
        cli_error("cli.missing_option_value", "An option is missing its value."));
  }
  ++index;
  return Result<std::wstring_view>::success(arguments[index]);
}

template <typename T>
Result<CliArguments> propagate(const Result<T>& result) {
  return Result<CliArguments>::failure(result.error());
}

bool same_luid(LUID left, LUID right) noexcept {
  return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

}  // namespace

Result<CliArguments> parse_arguments(std::span<const std::wstring_view> arguments) {
  if (arguments.empty()) {
    return Result<CliArguments>::failure(
        cli_error("cli.missing_command", "A command is required."));
  }

  CliArguments result;
  if (arguments[0] == L"doctor-json") {
    result.command = CliCommand::DoctorJson;
  } else if (arguments[0] == L"catalog-json") {
    result.command = CliCommand::CatalogJson;
  } else if (arguments[0] == L"test-json") {
    result.command = CliCommand::TestJson;
  } else if (arguments[0] == L"session-json") {
    result.command = CliCommand::SessionJson;
  } else if (arguments[0] == L"doctor") {
    result.command = CliCommand::Doctor;
  } else if (arguments[0] == L"list-monitors") {
    result.command = CliCommand::ListMonitors;
  } else if (arguments[0] == L"list-audio-devices") {
    result.command = CliCommand::ListAudioDevices;
  } else if (arguments[0] == L"capture") {
    result.command = CliCommand::Capture;
  } else if (arguments[0] == L"smoke") {
    result.command = CliCommand::Smoke;
  } else if (arguments[0] == L"session") {
    result.command = CliCommand::Session;
  } else {
    return Result<CliArguments>::failure(
        cli_error("cli.unknown_command", "The requested command is not supported."));
  }

  std::unordered_set<std::wstring> seen;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const auto option = arguments[index];
    if (!is_option(option)) {
      return Result<CliArguments>::failure(
          cli_error("cli.unexpected_argument", "A positional argument was not expected."));
    }
    if (!seen.emplace(option).second) {
      return Result<CliArguments>::failure(
          cli_error("cli.duplicate_option", "An option was provided more than once."));
    }
    auto value = option_value(arguments, index);
    if (!value.is_success()) {
      return propagate(value);
    }

    const bool capture_or_session = result.command == CliCommand::Capture ||
                                    result.command == CliCommand::Session;
    if (option == L"--config" && (result.command == CliCommand::TestJson ||
                                 result.command == CliCommand::SessionJson)) {
      result.config_path = std::filesystem::path(value.value());
    } else if (option == L"--monitor" && (capture_or_session || result.command == CliCommand::Doctor || result.command == CliCommand::DoctorJson)) {
      auto parsed = parse_monitor(value.value());
      if (!parsed.is_success()) return propagate(parsed);
      result.monitor = parsed.value();
    } else if (option == L"--microphone-id" && result.command == CliCommand::Session) {
      result.microphone_id = value.value();
    } else if (option == L"--width" && capture_or_session) {
      auto parsed = bounded_decimal(value.value(), 64, 16'384);
      if (!parsed.is_success()) return propagate(parsed);
      result.width = parsed.value();
    } else if (option == L"--height" && capture_or_session) {
      auto parsed = bounded_decimal(value.value(), 64, 16'384);
      if (!parsed.is_success()) return propagate(parsed);
      result.height = parsed.value();
    } else if (option == L"--fps" && capture_or_session) {
      auto parsed = bounded_decimal(value.value(), 1, 240);
      if (!parsed.is_success()) return propagate(parsed);
      result.fps = parsed.value();
    } else if (option == L"--bitrate" && capture_or_session) {
      auto parsed = bounded_decimal(value.value(), 100'000, 500'000'000);
      if (!parsed.is_success()) return propagate(parsed);
      result.bitrate = parsed.value();
    } else if (option == L"--buffer-seconds" && capture_or_session) {
      auto parsed = bounded_decimal(value.value(), 1,
                                    (std::numeric_limits<std::uint32_t>::max)());
      if (!parsed.is_success()) return propagate(parsed);
      result.buffer_seconds = parsed.value();
    } else if (option == L"--clip-seconds" && capture_or_session) {
      auto parsed = bounded_decimal(value.value(), 1,
                                    (std::numeric_limits<std::uint32_t>::max)());
      if (!parsed.is_success()) return propagate(parsed);
      result.clip_seconds = parsed.value();
    } else if (option == L"--duration-seconds" && result.command == CliCommand::Capture) {
      auto parsed = bounded_decimal(value.value(), 1,
                                    (std::numeric_limits<std::uint32_t>::max)());
      if (!parsed.is_success()) return propagate(parsed);
      result.duration_seconds = parsed.value();
    } else if (option == L"--seconds" && result.command == CliCommand::Smoke) {
      auto parsed = bounded_decimal(value.value(), 1,
                                    (std::numeric_limits<std::uint32_t>::max)());
      if (!parsed.is_success()) return propagate(parsed);
      result.seconds = parsed.value();
    } else if (option == L"--container" &&
               (capture_or_session || result.command == CliCommand::Smoke)) {
      if (value.value() == L"mp4") {
        result.container = Container::Mp4;
      } else if (value.value() == L"mkv") {
        result.container = Container::Mkv;
      } else {
        return Result<CliArguments>::failure(
            cli_error("cli.invalid_option_value", "Container must be mp4 or mkv."));
      }
    } else if (option == L"--output" &&
               (capture_or_session || result.command == CliCommand::Smoke)) {
      result.output = std::filesystem::path(value.value());
    } else if (option == L"--hotkey" && result.command == CliCommand::Capture) {
      auto parsed = parse_hotkey(value.value());
      if (!parsed.is_success()) return propagate(parsed);
      result.hotkey = parsed.value();
    } else if (option == L"--replay-enabled" && result.command == CliCommand::Session) {
      if (value.value() == L"true") {
        result.replay_enabled = true;
      } else if (value.value() == L"false") {
        result.replay_enabled = false;
      } else {
        return Result<CliArguments>::failure(
            cli_error("cli.invalid_option_value", "Replay enabled must be true or false."));
      }
    } else if (option == L"--replay-hotkey" && result.command == CliCommand::Session) {
      auto parsed = parse_hotkey_chord(value.value());
      if (!parsed.is_success()) return propagate(parsed);
      result.replay_hotkey = parsed.value();
    } else if (option == L"--recording-hotkey" && result.command == CliCommand::Session) {
      auto parsed = parse_hotkey_chord(value.value());
      if (!parsed.is_success()) return propagate(parsed);
      result.recording_hotkey = parsed.value();
    } else {
      return Result<CliArguments>::failure(
          cli_error("cli.unknown_option", "The option is not valid for this command."));
    }
  }

  if ((result.command == CliCommand::Capture || result.command == CliCommand::Session) &&
      !result.monitor.has_value()) {
    return Result<CliArguments>::failure(
        cli_error("cli.missing_required_option", "This command requires --monitor."));
  }
  if (result.command == CliCommand::Session && result.microphone_id.empty()) {
    return Result<CliArguments>::failure(
        cli_error("cli.missing_required_option", "Session requires --microphone-id."));
  }
  if ((result.command == CliCommand::Capture || result.command == CliCommand::Smoke ||
       result.command == CliCommand::Session) &&
      result.output.empty()) {
    return Result<CliArguments>::failure(
        cli_error("cli.missing_required_option", "This command requires --output."));
  }
  if ((result.command == CliCommand::Capture || result.command == CliCommand::Session) &&
      result.clip_seconds > result.buffer_seconds) {
    return Result<CliArguments>::failure(
        cli_error("cli.invalid_option_value", "Clip duration cannot exceed replay capacity."));
  }
  if (result.command == CliCommand::Session &&
      result.replay_hotkey == result.recording_hotkey) {
    return Result<CliArguments>::failure(
        cli_error("hotkey.duplicate_chord", "Replay and recording hotkeys must be distinct."));
  }
  if ((result.command == CliCommand::TestJson || result.command == CliCommand::SessionJson) &&
      result.config_path.empty()) {
    return Result<CliArguments>::failure(cli_error("cli.missing_required_option", "Native mode requires --config."));
  }
  return Result<CliArguments>::success(std::move(result));
}

Result<MonitorInfo> select_monitor(MonitorId requested,
                                   std::span<const MonitorInfo> monitors) {
  for (const auto& monitor : monitors) {
    if (same_luid(requested.adapter_luid, monitor.id.adapter_luid) &&
        requested.output_index == monitor.id.output_index) {
      return Result<MonitorInfo>::success(monitor);
    }
  }
  return Result<MonitorInfo>::failure(
      cli_error("cli.unknown_monitor", "The selected monitor is not in the current DXGI catalog."));
}

Result<MonitorInfo> select_diagnostic_monitor(std::optional<MonitorId> requested,
                                            std::span<const MonitorInfo> monitors) {
  if (requested) return select_monitor(*requested, monitors);
  for (const auto& monitor : monitors) if (monitor.primary) return Result<MonitorInfo>::success(monitor);
  if (!monitors.empty()) return Result<MonitorInfo>::success(monitors.front());
  return Result<MonitorInfo>::failure(cli_error("cli.unknown_monitor", "No monitor is attached."));
}

std::uint64_t monitor_id_value(MonitorId id) noexcept {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(id.adapter_luid.HighPart)) << 32U) |
         static_cast<std::uint32_t>(id.adapter_luid.LowPart);
}

std::string monitor_id_text(MonitorId id) {
  return std::to_string(monitor_id_value(id)) + ":" + std::to_string(id.output_index);
}

}  // namespace rebelliocap
