#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/result.h"

namespace rebelliocap {

struct HotkeyChord {
  std::uint16_t virtual_key;
  bool control{false};
  bool alt{false};
  bool shift{false};

  bool operator==(const HotkeyChord&) const = default;
};

enum class HotkeyAction { SaveReplay, ToggleRecording };

struct HotkeyBinding {
  HotkeyAction action;
  HotkeyChord chord;

  bool operator==(const HotkeyBinding&) const = default;
};

[[nodiscard]] Result<void> validate_hotkey_chord(HotkeyChord chord);
[[nodiscard]] Result<HotkeyChord> parse_hotkey_chord(std::wstring_view text);
[[nodiscard]] std::wstring format_hotkey_chord(HotkeyChord chord);

}  // namespace rebelliocap
