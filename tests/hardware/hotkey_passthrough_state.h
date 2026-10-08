#pragma once

#include <cstdint>
#include <mutex>

#include "core/qpc_clock.h"

namespace rebelliocap::hardware_test {

struct HotkeyPassthroughSnapshot {
  unsigned int foreground_key_downs{};
  unsigned int foreground_key_ups{};
  unsigned int foreground_repeats{};
  unsigned int unrelated_foreground_events{};
  unsigned int raw_input_presses{};
  bool focus_lost{};
  QpcTicks observation_started{};
  QpcTicks observation_finished{};
  QpcTicks foreground_down_qpc{};
  QpcTicks foreground_up_qpc{};
  QpcTicks raw_input_qpc{};
};

struct HotkeyPassthroughEvaluation {
  bool passed{};
  const char* reason{};
  HotkeyPassthroughSnapshot snapshot{};
};

class HotkeyPassthroughState final {
 public:
  explicit HotkeyPassthroughState(std::uint16_t expected_virtual_key) noexcept
      : expected_virtual_key_(expected_virtual_key) {}

  void begin(QpcTicks timestamp) noexcept {
    std::lock_guard lock(mutex_);
    snapshot_ = {};
    snapshot_.observation_started = timestamp;
    observing_ = true;
  }

  void finish(QpcTicks timestamp) noexcept {
    std::lock_guard lock(mutex_);
    if (!observing_) {
      return;
    }
    snapshot_.observation_finished = timestamp;
    observing_ = false;
  }

  void record_foreground(std::uint16_t virtual_key, bool is_break, bool is_repeat,
                         QpcTicks timestamp) noexcept {
    std::lock_guard lock(mutex_);
    if (!observing_) {
      return;
    }
    if (virtual_key != expected_virtual_key_) {
      ++snapshot_.unrelated_foreground_events;
      return;
    }
    if (is_break) {
      ++snapshot_.foreground_key_ups;
      if (snapshot_.foreground_key_ups == 1) {
        snapshot_.foreground_up_qpc = timestamp;
      }
      return;
    }

    ++snapshot_.foreground_key_downs;
    if (is_repeat) {
      ++snapshot_.foreground_repeats;
    }
    if (snapshot_.foreground_key_downs == 1) {
      snapshot_.foreground_down_qpc = timestamp;
    }
  }

  void record_raw_press(QpcTicks timestamp) noexcept {
    std::lock_guard lock(mutex_);
    if (!observing_) {
      return;
    }
    ++snapshot_.raw_input_presses;
    if (snapshot_.raw_input_presses == 1) {
      snapshot_.raw_input_qpc = timestamp;
    }
  }

  void record_focus_loss() noexcept {
    std::lock_guard lock(mutex_);
    if (observing_) {
      snapshot_.focus_lost = true;
    }
  }

  [[nodiscard]] HotkeyPassthroughSnapshot snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    return snapshot_;
  }

  [[nodiscard]] HotkeyPassthroughEvaluation evaluate() const noexcept {
    const auto observed = snapshot();
    if (observed.observation_finished <= observed.observation_started) {
      return {false, "the bounded observation interval did not finish", observed};
    }
    if (observed.focus_lost) {
      return {false, "the test window lost foreground focus", observed};
    }
    if (observed.unrelated_foreground_events != 0) {
      return {false, "an unrelated foreground key event contaminated the probe", observed};
    }
    if (observed.foreground_repeats != 0) {
      return {false, "the key was held long enough to repeat", observed};
    }
    if (observed.foreground_key_downs != 1 || observed.foreground_key_ups != 1) {
      return {false, "the foreground did not receive exactly one make/break pair", observed};
    }
    if (observed.raw_input_presses != 1) {
      return {false, "the Raw Input observer did not accept exactly one press", observed};
    }
    if (observed.foreground_down_qpc < observed.observation_started ||
        observed.foreground_down_qpc > observed.observation_finished ||
        observed.foreground_up_qpc < observed.observation_started ||
        observed.foreground_up_qpc > observed.observation_finished ||
        observed.raw_input_qpc < observed.observation_started ||
        observed.raw_input_qpc > observed.observation_finished) {
      return {false, "an observation timestamp fell outside the bounded interval", observed};
    }
    if (observed.foreground_down_qpc > observed.foreground_up_qpc) {
      return {false, "the foreground break preceded its make", observed};
    }
    return {true, "one physical press reached foreground and Raw Input", observed};
  }

 private:
  const std::uint16_t expected_virtual_key_;
  mutable std::mutex mutex_;
  bool observing_{};
  HotkeyPassthroughSnapshot snapshot_{};
};

}  // namespace rebelliocap::hardware_test
