#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hotkey/hotkey_source.h"

namespace rebelliocap {

struct RawKeyboardEvent {
  std::uint16_t virtual_key;
  std::uint16_t make_code;
  bool e0;
  bool is_break;
  std::uintptr_t device;
  std::uintptr_t extra_information;
};

class HotkeyChordMatcher {
 public:
  explicit HotkeyChordMatcher(HotkeyChord chord) noexcept;

  [[nodiscard]] bool accept(RawKeyboardEvent event) noexcept;
  void remove_device(std::uintptr_t device) noexcept;
  void reset() noexcept;

 private:
  enum Modifier : std::uint8_t {
    LeftControl = 1U << 0U,
    RightControl = 1U << 1U,
    LeftAlt = 1U << 2U,
    RightAlt = 1U << 3U,
    LeftShift = 1U << 4U,
    RightShift = 1U << 5U,
  };

  struct DeviceState {
    std::uint8_t modifiers{0};
    bool armed{true};
  };

  HotkeyChord chord_;
  std::unordered_map<std::uintptr_t, DeviceState> devices_;
};

class RawInputHotkey final : public IHotkeySource {
 public:
  RawInputHotkey();
  ~RawInputHotkey() override;

  RawInputHotkey(const RawInputHotkey&) = delete;
  RawInputHotkey& operator=(const RawInputHotkey&) = delete;

  Result<void> start(std::span<const HotkeyBinding> bindings,
                     HotkeyCallback on_press) override;
  Result<void> start(std::initializer_list<HotkeyBinding> bindings,
                     HotkeyCallback on_press);
  void stop() noexcept override;
  [[nodiscard]] std::optional<Error> last_failure() const;

 private:
  bool post_test_event(RawKeyboardEvent event, QpcTicks timestamp);
  bool post_test_events(std::vector<std::pair<RawKeyboardEvent, QpcTicks>> events);
  [[nodiscard]] std::optional<std::uintptr_t> last_extra_information_for_test() const;

  friend class RawInputHotkeyTestAccess;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rebelliocap
