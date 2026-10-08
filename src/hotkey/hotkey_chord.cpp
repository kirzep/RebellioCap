#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "hotkey/hotkey_chord.h"

#include <optional>
#include <string>
#include <utility>

namespace rebelliocap {
namespace {

Error chord_error(std::string code, std::string message) {
  return {.code = std::move(code), .message = std::move(message), .hresult = std::nullopt};
}

wchar_t ascii_upper(wchar_t character) noexcept {
  if (character >= L'a' && character <= L'z') {
    return static_cast<wchar_t>(character - (L'a' - L'A'));
  }
  return character;
}

bool equals_ignoring_ascii_case(std::wstring_view left, std::wstring_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (ascii_upper(left[index]) != ascii_upper(right[index])) {
      return false;
    }
  }
  return true;
}

std::optional<std::uint16_t> parse_terminal_key(std::wstring_view token) noexcept {
  if (token.size() == 1) {
    const auto key = ascii_upper(token.front());
    if ((key >= L'A' && key <= L'Z') || (key >= L'0' && key <= L'9')) {
      return static_cast<std::uint16_t>(key);
    }
  }

  if (token.size() >= 2 && ascii_upper(token.front()) == L'F') {
    unsigned number = 0;
    for (const auto character : token.substr(1)) {
      if (character < L'0' || character > L'9') {
        number = 0;
        break;
      }
      number = number * 10U + static_cast<unsigned>(character - L'0');
      if (number > 24) {
        return std::nullopt;
      }
    }
    if (number >= 1 && number <= 24) {
      return static_cast<std::uint16_t>(VK_F1 + number - 1U);
    }
  }

  if (equals_ignoring_ascii_case(token, L"Tab")) return VK_TAB;
  if (equals_ignoring_ascii_case(token, L"Escape")) return VK_ESCAPE;
  if (equals_ignoring_ascii_case(token, L"Delete")) return VK_DELETE;
  if (equals_ignoring_ascii_case(token, L"Space")) return VK_SPACE;
  return std::nullopt;
}

std::wstring format_terminal_key(std::uint16_t virtual_key) {
  if ((virtual_key >= L'A' && virtual_key <= L'Z') ||
      (virtual_key >= L'0' && virtual_key <= L'9')) {
    return std::wstring(1, static_cast<wchar_t>(virtual_key));
  }
  if (virtual_key >= VK_F1 && virtual_key <= VK_F24) {
    return L"F" + std::to_wstring(virtual_key - VK_F1 + 1U);
  }
  switch (virtual_key) {
    case VK_TAB:
      return L"Tab";
    case VK_ESCAPE:
      return L"Escape";
    case VK_DELETE:
      return L"Delete";
    case VK_SPACE:
      return L"Space";
    default:
      return {};
  }
}

}  // namespace

Result<void> validate_hotkey_chord(HotkeyChord chord) {
  const bool reserved =
      (chord.virtual_key == VK_DELETE && chord.control && chord.alt) ||
      (chord.virtual_key == VK_TAB && chord.alt) ||
      (chord.virtual_key == VK_ESCAPE && (chord.control || chord.alt)) ||
      (chord.virtual_key == VK_SPACE && chord.alt) ||
      (chord.virtual_key == VK_F4 && chord.alt);
  if (reserved) {
    return Result<void>::failure(chord_error(
        "hotkey.reserved_chord", "The requested hotkey is reserved by Windows."));
  }
  if (chord.virtual_key == 0) {
    return Result<void>::failure(
        chord_error("hotkey.invalid_chord", "The hotkey must contain a key."));
  }
  return Result<void>::success();
}

Result<HotkeyChord> parse_hotkey_chord(std::wstring_view text) {
  if (text.empty()) {
    return Result<HotkeyChord>::failure(
        chord_error("hotkey.invalid_chord", "The hotkey chord is empty."));
  }

  HotkeyChord chord{.virtual_key = 0};
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto separator = text.find(L'+', begin);
    const auto terminal = separator == std::wstring_view::npos;
    const auto token = text.substr(begin, terminal ? text.size() - begin : separator - begin);
    if (token.empty()) {
      return Result<HotkeyChord>::failure(
          chord_error("hotkey.invalid_chord", "The hotkey contains an empty token."));
    }

    if (terminal) {
      const auto key = parse_terminal_key(token);
      if (!key.has_value()) {
        return Result<HotkeyChord>::failure(chord_error(
            "hotkey.invalid_chord", "The hotkey must end with a supported key."));
      }
      chord.virtual_key = *key;
      break;
    }

    bool* modifier = nullptr;
    if (equals_ignoring_ascii_case(token, L"Ctrl")) {
      modifier = &chord.control;
    } else if (equals_ignoring_ascii_case(token, L"Alt")) {
      modifier = &chord.alt;
    } else if (equals_ignoring_ascii_case(token, L"Shift")) {
      modifier = &chord.shift;
    }
    if (modifier == nullptr || *modifier) {
      return Result<HotkeyChord>::failure(chord_error(
          "hotkey.invalid_chord", "Each modifier may appear once before the terminal key."));
    }
    *modifier = true;
    begin = separator + 1;
  }

  const auto validated = validate_hotkey_chord(chord);
  if (!validated.is_success()) {
    return Result<HotkeyChord>::failure(validated.error());
  }
  return Result<HotkeyChord>::success(chord);
}

std::wstring format_hotkey_chord(HotkeyChord chord) {
  std::wstring formatted;
  const auto append = [&](std::wstring_view token) {
    if (!formatted.empty()) formatted += L'+';
    formatted += token;
  };
  if (chord.control) append(L"Ctrl");
  if (chord.alt) append(L"Alt");
  if (chord.shift) append(L"Shift");
  append(format_terminal_key(chord.virtual_key));
  return formatted;
}

}  // namespace rebelliocap
