#include <atomic>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>

#include "hotkey/raw_input_hotkey.h"
#include "../hardware/hotkey_passthrough_state.h"

namespace rebelliocap {

class RawInputHotkeyTestAccess {
 public:
  static bool post_event(RawInputHotkey& hotkey, RawKeyboardEvent event, QpcTicks timestamp) {
    return hotkey.post_test_event(event, timestamp);
  }

  static bool post_events(
      RawInputHotkey& hotkey,
      std::vector<std::pair<RawKeyboardEvent, QpcTicks>> events) {
    return hotkey.post_test_events(std::move(events));
  }

  static std::optional<std::uintptr_t> last_extra_information(
      const RawInputHotkey& hotkey) {
    return hotkey.last_extra_information_for_test();
  }
};

}  // namespace rebelliocap

namespace {

using rebelliocap::HotkeyChord;
using rebelliocap::HotkeyAction;
using rebelliocap::HotkeyChordMatcher;
using rebelliocap::RawKeyboardEvent;
using rebelliocap::RawInputHotkey;

RawKeyboardEvent key(std::uint16_t virtual_key, bool is_break = false,
                     bool e0 = false, std::uint16_t make_code = 0,
                     std::uintptr_t device = 1,
                     std::uintptr_t extra_information = 0) {
  return {.virtual_key = virtual_key,
          .make_code = make_code,
          .e0 = e0,
          .is_break = is_break,
          .device = device,
          .extra_information = extra_information};
}

rebelliocap::Result<void> start_save_replay(
    RawInputHotkey& hotkey, HotkeyChord chord,
    std::function<void(rebelliocap::QpcTicks)> callback) {
  const rebelliocap::HotkeyBinding binding{
      .action = HotkeyAction::SaveReplay, .chord = chord};
  return hotkey.start(std::span(&binding, 1),
                      [callback = std::move(callback)](
                          HotkeyAction, rebelliocap::QpcTicks timestamp) {
                        callback(timestamp);
                      });
}

}  // namespace

TEST_CASE("hotkey chords parse case-insensitively and format canonically") {
  const auto chord = rebelliocap::parse_hotkey_chord(L"shift+alt+f10");
  REQUIRE(chord.is_success());
  REQUIRE(chord.value().virtual_key == VK_F10);
  REQUIRE_FALSE(chord.value().control);
  REQUIRE(chord.value().alt);
  REQUIRE(chord.value().shift);
  REQUIRE(rebelliocap::format_hotkey_chord(chord.value()) == L"Alt+Shift+F10");
}

TEST_CASE("hotkey chord parsing is strict and preserves reserved-chord diagnostics") {
  const auto whitespace = rebelliocap::parse_hotkey_chord(L"Ctrl +R");
  REQUIRE_FALSE(whitespace.is_success());
  REQUIRE(whitespace.error().code == "hotkey.invalid_chord");

  const auto duplicate_modifier = rebelliocap::parse_hotkey_chord(L"Ctrl+ctrl+R");
  REQUIRE_FALSE(duplicate_modifier.is_success());
  REQUIRE(duplicate_modifier.error().code == "hotkey.invalid_chord");

  const auto non_terminal_key = rebelliocap::parse_hotkey_chord(L"R+Ctrl");
  REQUIRE_FALSE(non_terminal_key.is_success());
  REQUIRE(non_terminal_key.error().code == "hotkey.invalid_chord");

  const auto overflowing_function_key =
      rebelliocap::parse_hotkey_chord(L"F4294967297");
  REQUIRE_FALSE(overflowing_function_key.is_success());
  REQUIRE(overflowing_function_key.error().code == "hotkey.invalid_chord");

  const auto reserved = rebelliocap::parse_hotkey_chord(L"alt+f4");
  REQUIRE_FALSE(reserved.is_success());
  REQUIRE(reserved.error().code == "hotkey.reserved_chord");
}

TEST_CASE("either Control key satisfies the chord and releasing one preserves the other") {
  HotkeyChordMatcher matcher({.virtual_key = 'K', .control = true});

  REQUIRE_FALSE(matcher.accept(key(VK_CONTROL, false, false, 0x1D)));
  REQUIRE(matcher.accept(key('K')));
  REQUIRE_FALSE(matcher.accept(key('K')));
  REQUIRE_FALSE(matcher.accept(key('K', true)));
  REQUIRE_FALSE(matcher.accept(key(VK_CONTROL, true, false, 0x1D)));

  REQUIRE_FALSE(matcher.accept(key(VK_CONTROL, false, true, 0x1D)));
  REQUIRE(matcher.accept(key('K')));
  REQUIRE_FALSE(matcher.accept(key('K', true)));

  REQUIRE_FALSE(matcher.accept(key(VK_CONTROL, false, false, 0x1D)));
  REQUIRE_FALSE(matcher.accept(key(VK_CONTROL, true, true, 0x1D)));
  REQUIRE(matcher.accept(key('K')));
}

TEST_CASE("left and right Shift make codes independently maintain modifier state") {
  HotkeyChordMatcher matcher({.virtual_key = 'S', .shift = true});

  REQUIRE_FALSE(matcher.accept(key(VK_SHIFT, false, false, 0x2A)));
  REQUIRE_FALSE(matcher.accept(key(VK_SHIFT, false, false, 0x36)));
  REQUIRE_FALSE(matcher.accept(key(VK_SHIFT, true, false, 0x2A)));
  REQUIRE(matcher.accept(key('S')));
  REQUIRE_FALSE(matcher.accept(key('S', true)));
  REQUIRE_FALSE(matcher.accept(key(VK_SHIFT, true, false, 0x36)));
  REQUIRE_FALSE(matcher.accept(key('S')));
}

TEST_CASE("E0 key make fires once until its matching break rearms the chord") {
  HotkeyChordMatcher matcher({.virtual_key = VK_RIGHT});

  REQUIRE(matcher.accept(key(VK_RIGHT, false, true, 0x4D)));
  REQUIRE_FALSE(matcher.accept(key(VK_RIGHT, false, true, 0x4D)));
  REQUIRE_FALSE(matcher.accept(key(VK_RIGHT, true, true, 0x4D)));
  REQUIRE(matcher.accept(key(VK_RIGHT, false, true, 0x4D)));
}

TEST_CASE("a mismatched initial make cannot become a press through later autorepeat") {
  HotkeyChordMatcher matcher({.virtual_key = 'K', .control = true});

  REQUIRE_FALSE(matcher.accept(key('K')));
  REQUIRE_FALSE(matcher.accept(key(VK_CONTROL, false, false, 0x1D)));
  REQUIRE_FALSE(matcher.accept(key('K')));
  REQUIRE_FALSE(matcher.accept(key('K', true)));
  REQUIRE(matcher.accept(key('K')));
}

TEST_CASE("removing one keyboard cannot rearm a held key on another keyboard") {
  constexpr std::uintptr_t kKeyboardA = 101;
  constexpr std::uintptr_t kKeyboardB = 202;
  HotkeyChordMatcher matcher({.virtual_key = VK_F8});

  REQUIRE(matcher.accept(key(VK_F8, false, false, 0, kKeyboardA)));
  REQUIRE_FALSE(matcher.accept(key(VK_F8, false, false, 0, kKeyboardA)));

  REQUIRE_FALSE(matcher.accept(key(VK_SHIFT, false, false, 0x2A, kKeyboardB)));
  matcher.remove_device(kKeyboardB);
  REQUIRE_FALSE(matcher.accept(key(VK_F8, false, false, 0, kKeyboardA)));

  REQUIRE_FALSE(matcher.accept(key(VK_F8, true, false, 0, kKeyboardA)));
  REQUIRE(matcher.accept(key(VK_F8, false, false, 0, kKeyboardA)));
}

TEST_CASE("modifiers aggregate across keyboards and removal subtracts only one contribution") {
  constexpr std::uintptr_t kKeyboardA = 301;
  constexpr std::uintptr_t kKeyboardB = 302;
  constexpr std::uintptr_t kKeyboardC = 303;
  HotkeyChordMatcher matcher({.virtual_key = 'K', .control = true});

  REQUIRE_FALSE(matcher.accept(key(VK_CONTROL, false, false, 0x1D, kKeyboardA)));
  REQUIRE(matcher.accept(key('K', false, false, 0, kKeyboardB)));
  REQUIRE_FALSE(matcher.accept(key('K', true, false, 0, kKeyboardB)));

  REQUIRE_FALSE(matcher.accept(key(VK_CONTROL, false, true, 0x1D, kKeyboardC)));
  matcher.remove_device(kKeyboardA);
  REQUIRE(matcher.accept(key('K', false, false, 0, kKeyboardB)));
  REQUIRE_FALSE(matcher.accept(key('K', true, false, 0, kKeyboardB)));

  matcher.remove_device(kKeyboardC);
  REQUIRE_FALSE(matcher.accept(key('K', false, false, 0, kKeyboardB)));
}

TEST_CASE("Alt uses E0 to distinguish right from left without losing held state") {
  HotkeyChordMatcher matcher({.virtual_key = 'A', .alt = true});

  REQUIRE_FALSE(matcher.accept(key(VK_MENU, false, false, 0x38)));
  REQUIRE_FALSE(matcher.accept(key(VK_MENU, false, true, 0x38)));
  REQUIRE_FALSE(matcher.accept(key(VK_MENU, true, false, 0x38)));
  REQUIRE(matcher.accept(key('A')));
}

TEST_CASE("reserved security and shell combinations are rejected before observation") {
  const HotkeyChord reserved[] = {
      {.virtual_key = VK_DELETE, .control = true, .alt = true},
      {.virtual_key = VK_TAB, .alt = true},
      {.virtual_key = VK_ESCAPE, .control = true},
      {.virtual_key = VK_ESCAPE, .alt = true},
      {.virtual_key = VK_SPACE, .alt = true},
      {.virtual_key = VK_F4, .alt = true},
  };

  for (const auto chord : reserved) {
    const auto result = rebelliocap::validate_hotkey_chord(chord);
    REQUIRE_FALSE(result.is_success());
    REQUIRE(result.error().code == "hotkey.reserved_chord");
  }

  REQUIRE(rebelliocap::validate_hotkey_chord(
              {.virtual_key = VK_F8, .control = true, .shift = true})
              .is_success());
}

TEST_CASE("duplicate normalized chords are rejected before observation") {
  RawInputHotkey observer;
  const std::vector<rebelliocap::HotkeyBinding> bindings{
      {.action = HotkeyAction::SaveReplay,
       .chord = {.virtual_key = VK_F10, .alt = true}},
      {.action = HotkeyAction::ToggleRecording,
       .chord = {.virtual_key = VK_F10, .alt = true}},
  };

  const auto started = observer.start(bindings, [](HotkeyAction, rebelliocap::QpcTicks) {});
  REQUIRE_FALSE(started.is_success());
  REQUIRE(started.error().code == "hotkey.duplicate_chord");
}

TEST_CASE("two configured chords dispatch their own actions once") {
  RawInputHotkey observer;
  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  std::vector<HotkeyAction> actions;

  REQUIRE(observer.start(
      {{.action = HotkeyAction::SaveReplay,
        .chord = {.virtual_key = VK_F10, .alt = true}},
       {.action = HotkeyAction::ToggleRecording,
        .chord = {.virtual_key = 'R', .control = true, .shift = true}}},
      [&](HotkeyAction action, rebelliocap::QpcTicks) {
            {
              std::lock_guard lock(completion_mutex);
              actions.push_back(action);
            }
            completion_cv.notify_one();
          }).is_success());
  REQUIRE(rebelliocap::RawInputHotkeyTestAccess::post_events(
      observer,
      {{key(VK_MENU, false, false, 0x38), 100},
       {key(VK_F10), 101},
       {key(VK_F10, true), 102},
       {key(VK_MENU, true, false, 0x38), 103},
       {key(VK_CONTROL, false, false, 0x1D), 200},
       {key(VK_SHIFT, false, false, 0x2A), 201},
       {key('R'), 202},
       {key('R', true), 203},
       {key(VK_SHIFT, true, false, 0x2A), 204},
       {key(VK_CONTROL, true, false, 0x1D), 205}}));

  std::unique_lock completion_lock(completion_mutex);
  REQUIRE(completion_cv.wait_for(completion_lock, std::chrono::seconds(2),
                                 [&] { return actions.size() == 2; }));
  REQUIRE(actions == std::vector{HotkeyAction::SaveReplay,
                                 HotkeyAction::ToggleRecording});
  completion_lock.unlock();
  observer.stop();
}

TEST_CASE("only one Raw Input keyboard registrar can be active in the process") {
  RawInputHotkey first;
  RawInputHotkey second;

  const auto first_start =
      start_save_replay(first, {.virtual_key = VK_F24}, [](rebelliocap::QpcTicks) {});
  REQUIRE(first_start.is_success());

  const auto competing_start =
      start_save_replay(second, {.virtual_key = VK_F23}, [](rebelliocap::QpcTicks) {});
  REQUIRE_FALSE(competing_start.is_success());
  REQUIRE(competing_start.error().code == "hotkey.registration_in_use");

  first.stop();
  const auto second_start =
      start_save_replay(second, {.virtual_key = VK_F23}, [](rebelliocap::QpcTicks) {});
  REQUIRE(second_start.is_success());
  second.stop();
}

TEST_CASE("callback can stop and destroy its Raw Input hotkey owner on the worker thread") {
  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  bool callback_completed = false;
  std::atomic_uint callback_count{0};
  auto owner = std::make_unique<RawInputHotkey>();
  auto* observer = owner.get();

  const auto started = start_save_replay(*owner, {.virtual_key = VK_F24},
                                         [&](rebelliocap::QpcTicks) {
    const auto callback_index = callback_count.fetch_add(1, std::memory_order_acq_rel);
    if (callback_index == 0) {
      owner->stop();
      owner.reset();
      {
        std::lock_guard lock(completion_mutex);
        callback_completed = true;
      }
      completion_cv.notify_one();
    }
  });
  REQUIRE(started.is_success());
  REQUIRE(rebelliocap::RawInputHotkeyTestAccess::post_events(
      *observer,
      {{key(VK_F24), 123},
       {key(VK_F24, true), 124},
       {key(VK_F24), 125},
       {key(VK_F24, true), 126}}));

  std::unique_lock completion_lock(completion_mutex);
  REQUIRE(completion_cv.wait_for(completion_lock, std::chrono::seconds(2),
                                 [&] { return callback_completed; }));
  completion_lock.unlock();
  REQUIRE(owner == nullptr);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(callback_count.load(std::memory_order_acquire) == 1);

  RawInputHotkey replacement;
  bool replacement_started = false;
  for (int attempt = 0; attempt < 100 && !replacement_started; ++attempt) {
    const auto result = start_save_replay(
        replacement, {.virtual_key = VK_F24}, [](rebelliocap::QpcTicks) {});
    replacement_started = result.is_success();
    if (!replacement_started) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  REQUIRE(replacement_started);
  replacement.stop();
}

TEST_CASE("accepted observation preserves Raw Input extra information for the test seam") {
  constexpr std::uintptr_t kExtraInformation = 0x53434C50U;
  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  bool callback_completed = false;
  RawInputHotkey observer;

  REQUIRE(start_save_replay(observer, {.virtual_key = VK_F21},
                            [&](rebelliocap::QpcTicks) {
            {
              std::lock_guard lock(completion_mutex);
              callback_completed = true;
            }
            completion_cv.notify_one();
          }).is_success());
  REQUIRE(rebelliocap::RawInputHotkeyTestAccess::post_event(
      observer, key(VK_F21, false, false, 0, 501, kExtraInformation), 789));

  std::unique_lock completion_lock(completion_mutex);
  REQUIRE(completion_cv.wait_for(completion_lock, std::chrono::seconds(2),
                                 [&] { return callback_completed; }));
  const auto observed_extra =
      rebelliocap::RawInputHotkeyTestAccess::last_extra_information(observer);
  REQUIRE(observed_extra.has_value());
  REQUIRE(*observed_extra == kExtraInformation);
  completion_lock.unlock();
  observer.stop();
}

TEST_CASE("registered hotkey message dispatches when Raw Input is unavailable") {
  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  bool callback_completed = false;
  rebelliocap::QpcTicks observed_timestamp = 0;
  HotkeyAction observed_action = HotkeyAction::SaveReplay;
  RawInputHotkey observer;
  const rebelliocap::HotkeyBinding binding{
      .action = HotkeyAction::ToggleRecording,
      .chord = {.virtual_key = VK_F24}};

  REQUIRE(observer.start(std::span(&binding, 1),
                         [&](HotkeyAction action, rebelliocap::QpcTicks timestamp) {
            {
              std::lock_guard lock(completion_mutex);
              callback_completed = true;
              observed_action = action;
              observed_timestamp = timestamp;
            }
            completion_cv.notify_one();
          }).is_success());

  const auto message_window =
      FindWindowExW(HWND_MESSAGE, nullptr, L"RebellioCapRawInputHotkey", L"");
  REQUIRE(message_window != nullptr);
  REQUIRE(PostMessageW(message_window, WM_HOTKEY, 0x5343,
                       MAKELPARAM(MOD_NOREPEAT, VK_F24)) != FALSE);

  std::unique_lock completion_lock(completion_mutex);
  REQUIRE(completion_cv.wait_for(completion_lock, std::chrono::seconds(2),
                                 [&] { return callback_completed; }));
  REQUIRE(observed_action == HotkeyAction::ToggleRecording);
  REQUIRE(observed_timestamp > 0);
  completion_lock.unlock();
  observer.stop();
}

TEST_CASE("queued fallback messages cannot dispatch after the first callback stops its owner") {
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool first_callback_entered = false;
  bool second_message_queued = false;
  bool first_callback_completed = false;
  std::atomic_uint callback_count{0};
  auto owner = std::make_unique<RawInputHotkey>();
  const rebelliocap::HotkeyBinding binding{
      .action = HotkeyAction::ToggleRecording,
      .chord = {.virtual_key = VK_F17}};

  REQUIRE(owner->start(std::span(&binding, 1),
                       [&](HotkeyAction, rebelliocap::QpcTicks) {
                         const auto callback_index =
                             callback_count.fetch_add(1, std::memory_order_acq_rel);
                         if (callback_index != 0) {
                           return;
                         }
                         {
                           std::unique_lock gate_lock(gate_mutex);
                           first_callback_entered = true;
                           gate_cv.notify_one();
                           gate_cv.wait(gate_lock, [&] { return second_message_queued; });
                         }
                         owner->stop();
                         owner.reset();
                         {
                           std::lock_guard gate_lock(gate_mutex);
                           first_callback_completed = true;
                         }
                         gate_cv.notify_one();
                       }).is_success());
  const auto message_window =
      FindWindowExW(HWND_MESSAGE, nullptr, L"RebellioCapRawInputHotkey", L"");
  REQUIRE(message_window != nullptr);
  REQUIRE(PostMessageW(message_window, WM_HOTKEY, 0x5343,
                       MAKELPARAM(MOD_NOREPEAT, VK_F17)) != FALSE);

  {
    std::unique_lock gate_lock(gate_mutex);
    REQUIRE(gate_cv.wait_for(gate_lock, std::chrono::seconds(2),
                             [&] { return first_callback_entered; }));
  }
  REQUIRE(PostMessageW(message_window, WM_HOTKEY, 0x5343,
                       MAKELPARAM(MOD_NOREPEAT, VK_F17)) != FALSE);
  {
    std::lock_guard gate_lock(gate_mutex);
    second_message_queued = true;
  }
  gate_cv.notify_one();

  {
    std::unique_lock gate_lock(gate_mutex);
    REQUIRE(gate_cv.wait_for(gate_lock, std::chrono::seconds(2),
                             [&] { return first_callback_completed; }));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  REQUIRE(owner == nullptr);
  REQUIRE(callback_count.load(std::memory_order_acquire) == 1);
}

TEST_CASE("fallback owns and releases the configured Windows hotkey") {
  constexpr int kCompetingId = 0x5344;
  RawInputHotkey observer;
  REQUIRE(start_save_replay(observer, {.virtual_key = VK_F22},
                            [](rebelliocap::QpcTicks) {})
              .is_success());

  SetLastError(ERROR_SUCCESS);
  const bool competing_registration =
      RegisterHotKey(nullptr, kCompetingId, MOD_NOREPEAT, VK_F22) != FALSE;
  const auto competing_error = GetLastError();
  if (competing_registration) {
    UnregisterHotKey(nullptr, kCompetingId);
  }
  REQUIRE_FALSE(competing_registration);
  REQUIRE(competing_error == ERROR_HOTKEY_ALREADY_REGISTERED);

  observer.stop();
  REQUIRE(RegisterHotKey(nullptr, kCompetingId, MOD_NOREPEAT, VK_F22) != FALSE);
  REQUIRE(UnregisterHotKey(nullptr, kCompetingId) != FALSE);
}

TEST_CASE("fallback registration failure retains its Win32 error and leaves Raw Input active") {
  constexpr int kCompetingId = 0x5345;
  REQUIRE(RegisterHotKey(nullptr, kCompetingId, MOD_NOREPEAT, VK_F20) != FALSE);

  RawInputHotkey observer;
  const auto started = start_save_replay(
      observer, {.virtual_key = VK_F20}, [](rebelliocap::QpcTicks) {});
  const auto failure = observer.last_failure();
  observer.stop();
  REQUIRE(UnregisterHotKey(nullptr, kCompetingId) != FALSE);

  REQUIRE(started.is_success());
  REQUIRE(failure.has_value());
  REQUIRE(failure->code == "hotkey.fallback_registration_failed");
  REQUIRE(failure->hresult == ERROR_HOTKEY_ALREADY_REGISTERED);
}

TEST_CASE("one physical press observed by both backends dispatches only once") {
  std::atomic_uint callback_count{0};
  std::atomic<HotkeyAction> observed_action{HotkeyAction::ToggleRecording};
  RawInputHotkey observer;
  const rebelliocap::HotkeyBinding binding{
      .action = HotkeyAction::SaveReplay,
      .chord = {.virtual_key = VK_F23}};

  REQUIRE(observer.start(std::span(&binding, 1),
                         [&](HotkeyAction action, rebelliocap::QpcTicks) {
            observed_action.store(action, std::memory_order_release);
            callback_count.fetch_add(1, std::memory_order_acq_rel);
          }).is_success());

  const auto message_window =
      FindWindowExW(HWND_MESSAGE, nullptr, L"RebellioCapRawInputHotkey", L"");
  REQUIRE(message_window != nullptr);
  LARGE_INTEGER observed{};
  REQUIRE(QueryPerformanceCounter(&observed) != FALSE);
  REQUIRE(rebelliocap::RawInputHotkeyTestAccess::post_event(
      observer, key(VK_F23), observed.QuadPart));
  REQUIRE(PostMessageW(message_window, WM_HOTKEY, 0x5343,
                       MAKELPARAM(MOD_NOREPEAT, VK_F23)) != FALSE);

  for (int attempt = 0; attempt < 100 && callback_count.load(std::memory_order_acquire) < 2;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(callback_count.load(std::memory_order_acquire) == 1);
  REQUIRE(observed_action.load(std::memory_order_acquire) == HotkeyAction::SaveReplay);
  observer.stop();
}

TEST_CASE("slow callbacks preserve cross-backend deduplication in either arrival order") {
  const auto run_order = [](bool raw_input_first) {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool first_callback_entered = false;
    bool release_first_callback = false;
    bool first_callback_completed = false;
    std::atomic_uint callback_count{0};
    RawInputHotkey observer;
    const rebelliocap::HotkeyBinding binding{
        .action = HotkeyAction::SaveReplay,
        .chord = {.virtual_key = VK_F18}};

    REQUIRE(observer.start(std::span(&binding, 1),
                           [&](HotkeyAction, rebelliocap::QpcTicks) {
                             const auto callback_index =
                                 callback_count.fetch_add(1, std::memory_order_acq_rel);
                             if (callback_index != 0) {
                               return;
                             }
                             {
                               std::unique_lock gate_lock(gate_mutex);
                               first_callback_entered = true;
                               gate_cv.notify_one();
                               gate_cv.wait(gate_lock,
                                            [&] { return release_first_callback; });
                             }
                             std::this_thread::sleep_for(std::chrono::milliseconds(150));
                             {
                               std::lock_guard gate_lock(gate_mutex);
                               first_callback_completed = true;
                             }
                             gate_cv.notify_one();
                           }).is_success());
    const auto message_window =
        FindWindowExW(HWND_MESSAGE, nullptr, L"RebellioCapRawInputHotkey", L"");
    REQUIRE(message_window != nullptr);

    LARGE_INTEGER first_observed{};
    REQUIRE(QueryPerformanceCounter(&first_observed) != FALSE);
    if (raw_input_first) {
      REQUIRE(rebelliocap::RawInputHotkeyTestAccess::post_events(
          observer, {{key(VK_F18), first_observed.QuadPart},
                     {key(VK_F18, true), first_observed.QuadPart + 1}}));
    } else {
      REQUIRE(PostMessageW(message_window, WM_HOTKEY, 0x5343,
                           MAKELPARAM(MOD_NOREPEAT, VK_F18)) != FALSE);
    }

    {
      std::unique_lock gate_lock(gate_mutex);
      REQUIRE(gate_cv.wait_for(gate_lock, std::chrono::seconds(2),
                               [&] { return first_callback_entered; }));
    }

    LARGE_INTEGER second_observed{};
    REQUIRE(QueryPerformanceCounter(&second_observed) != FALSE);
    if (raw_input_first) {
      REQUIRE(PostMessageW(message_window, WM_HOTKEY, 0x5343,
                           MAKELPARAM(MOD_NOREPEAT, VK_F18)) != FALSE);
    } else {
      REQUIRE(rebelliocap::RawInputHotkeyTestAccess::post_events(
          observer, {{key(VK_F18), second_observed.QuadPart},
                     {key(VK_F18, true), second_observed.QuadPart + 1}}));
    }
    {
      std::lock_guard gate_lock(gate_mutex);
      release_first_callback = true;
    }
    gate_cv.notify_one();

    {
      std::unique_lock gate_lock(gate_mutex);
      REQUIRE(gate_cv.wait_for(gate_lock, std::chrono::seconds(2),
                               [&] { return first_callback_completed; }));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(callback_count.load(std::memory_order_acquire) == 1);
    observer.stop();
  };

  SECTION("Raw Input arrives before WM_HOTKEY") { run_order(true); }
  SECTION("WM_HOTKEY arrives before Raw Input") { run_order(false); }
}

TEST_CASE("a suppressed backend duplicate does not hide the next physical press") {
  std::atomic_uint callback_count{0};
  RawInputHotkey observer;
  const rebelliocap::HotkeyBinding binding{
      .action = HotkeyAction::SaveReplay,
      .chord = {.virtual_key = VK_F19}};

  REQUIRE(observer.start(std::span(&binding, 1),
                         [&](HotkeyAction, rebelliocap::QpcTicks) {
                           callback_count.fetch_add(1, std::memory_order_acq_rel);
                         }).is_success());
  const auto message_window =
      FindWindowExW(HWND_MESSAGE, nullptr, L"RebellioCapRawInputHotkey", L"");
  REQUIRE(message_window != nullptr);

  const auto post_physical_press = [&] {
    LARGE_INTEGER observed{};
    REQUIRE(QueryPerformanceCounter(&observed) != FALSE);
    REQUIRE(rebelliocap::RawInputHotkeyTestAccess::post_events(
        observer, {{key(VK_F19), observed.QuadPart},
                   {key(VK_F19, true), observed.QuadPart + 1}}));
    REQUIRE(PostMessageW(message_window, WM_HOTKEY, 0x5343,
                         MAKELPARAM(MOD_NOREPEAT, VK_F19)) != FALSE);
  };

  post_physical_press();
  for (int attempt = 0; attempt < 100 && callback_count.load(std::memory_order_acquire) < 1;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(callback_count.load(std::memory_order_acquire) == 1);

  post_physical_press();
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  REQUIRE(callback_count.load(std::memory_order_acquire) == 2);
  observer.stop();
}

TEST_CASE("opposite backends do not deduplicate different actions") {
  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  std::vector<HotkeyAction> actions;
  RawInputHotkey observer;
  const std::vector<rebelliocap::HotkeyBinding> bindings{
      {.action = HotkeyAction::SaveReplay,
       .chord = {.virtual_key = VK_F23}},
      {.action = HotkeyAction::ToggleRecording,
       .chord = {.virtual_key = VK_F24}},
  };

  REQUIRE(observer.start(bindings, [&](HotkeyAction action, rebelliocap::QpcTicks) {
            {
              std::lock_guard lock(completion_mutex);
              actions.push_back(action);
            }
            completion_cv.notify_one();
          }).is_success());
  const auto message_window =
      FindWindowExW(HWND_MESSAGE, nullptr, L"RebellioCapRawInputHotkey", L"");
  REQUIRE(message_window != nullptr);
  LARGE_INTEGER observed{};
  REQUIRE(QueryPerformanceCounter(&observed) != FALSE);
  REQUIRE(rebelliocap::RawInputHotkeyTestAccess::post_event(
      observer, key(VK_F23), observed.QuadPart));
  REQUIRE(PostMessageW(message_window, WM_HOTKEY, 0x5344,
                       MAKELPARAM(MOD_NOREPEAT, VK_F24)) != FALSE);

  std::unique_lock completion_lock(completion_mutex);
  REQUIRE(completion_cv.wait_for(completion_lock, std::chrono::seconds(2),
                                 [&] { return actions.size() == 2; }));
  REQUIRE(actions == std::vector{HotkeyAction::SaveReplay,
                                 HotkeyAction::ToggleRecording});
  completion_lock.unlock();
  observer.stop();
}

TEST_CASE("throwing callback is contained, diagnosed, and shuts down its registrar") {
  RawInputHotkey observer;
  const auto started = start_save_replay(
      observer, {.virtual_key = VK_F23}, [](rebelliocap::QpcTicks) {
        throw std::runtime_error("callback failure sentinel");
      });
  REQUIRE(started.is_success());
  REQUIRE(rebelliocap::RawInputHotkeyTestAccess::post_event(
      observer, key(VK_F23), 456));

  std::optional<rebelliocap::Error> failure;
  for (int attempt = 0; attempt < 100 && !failure.has_value(); ++attempt) {
    failure = observer.last_failure();
    if (!failure.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  REQUIRE(failure.has_value());
  REQUIRE(failure->code == "hotkey.callback_exception");
  REQUIRE(failure->message.find("callback failure sentinel") != std::string::npos);
  observer.stop();

  RawInputHotkey replacement;
  REQUIRE(start_save_replay(replacement, {.virtual_key = VK_F22},
                            [](rebelliocap::QpcTicks) {})
              .is_success());
  replacement.stop();
}

TEST_CASE("physical pass-through probe accepts one complete foreground and Raw Input press") {
  rebelliocap::hardware_test::HotkeyPassthroughState state('K');

  state.begin(100);
  state.record_foreground('K', false, false, 110);
  state.record_raw_press(115);
  state.record_foreground('K', true, false, 120);
  state.finish(130);

  const auto result = state.evaluate();
  REQUIRE(result.passed);
  REQUIRE(result.snapshot.foreground_key_downs == 1);
  REQUIRE(result.snapshot.foreground_key_ups == 1);
  REQUIRE(result.snapshot.raw_input_presses == 1);
  REQUIRE(result.snapshot.foreground_down_qpc == 110);
  REQUIRE(result.snapshot.foreground_up_qpc == 120);
  REQUIRE(result.snapshot.raw_input_qpc == 115);
}

TEST_CASE("physical pass-through probe rejects contaminated or incomplete observations") {
  using rebelliocap::hardware_test::HotkeyPassthroughState;

  SECTION("unrelated foreground key") {
    HotkeyPassthroughState state('K');
    state.begin(100);
    state.record_foreground('J', false, false, 105);
    state.record_foreground('K', false, false, 110);
    state.record_raw_press(115);
    state.record_foreground('K', true, false, 120);
    state.finish(130);
    REQUIRE_FALSE(state.evaluate().passed);
  }

  SECTION("repeat or second make") {
    HotkeyPassthroughState state('K');
    state.begin(100);
    state.record_foreground('K', false, false, 110);
    state.record_foreground('K', false, true, 111);
    state.record_raw_press(115);
    state.record_foreground('K', true, false, 120);
    state.finish(130);
    REQUIRE_FALSE(state.evaluate().passed);
  }

  SECTION("focus loss") {
    HotkeyPassthroughState state('K');
    state.begin(100);
    state.record_foreground('K', false, false, 110);
    state.record_raw_press(115);
    state.record_focus_loss();
    state.record_foreground('K', true, false, 120);
    state.finish(130);
    REQUIRE_FALSE(state.evaluate().passed);
  }

  SECTION("missing foreground break") {
    HotkeyPassthroughState state('K');
    state.begin(100);
    state.record_foreground('K', false, false, 110);
    state.record_raw_press(115);
    state.finish(130);
    REQUIRE_FALSE(state.evaluate().passed);
  }

  SECTION("missing Raw Input press") {
    HotkeyPassthroughState state('K');
    state.begin(100);
    state.record_foreground('K', false, false, 110);
    state.record_foreground('K', true, false, 120);
    state.finish(130);
    REQUIRE_FALSE(state.evaluate().passed);
  }
}
