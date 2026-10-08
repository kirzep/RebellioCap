#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <thread>

#include <Windows.h>

#include "hotkey/raw_input_hotkey.h"
#include "hotkey_passthrough_state.h"

namespace {

constexpr int kUnavailable = 77;
constexpr wchar_t kOptInVariable[] = L"REBELLIOCAP_RUN_INTERACTIVE_HOTKEY_PROBE";
constexpr wchar_t kWindowClass[] = L"RebellioCapHotkeyPassthroughTest";
constexpr WORD kTestVirtualKey = 'K';
constexpr auto kInteractionTimeout = std::chrono::seconds(15);
constexpr auto kStabilityInterval = std::chrono::milliseconds(300);

struct WindowContext {
  rebelliocap::hardware_test::HotkeyPassthroughState* state{};
  std::atomic_bool monitoring{};
};

bool interactive_probe_enabled() noexcept {
  wchar_t value[2]{};
  return GetEnvironmentVariableW(kOptInVariable, value, 2) == 1 && value[0] == L'1';
}

rebelliocap::QpcTicks qpc_now() noexcept {
  LARGE_INTEGER observed{};
  QueryPerformanceCounter(&observed);
  return observed.QuadPart;
}

LRESULT CALLBACK test_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
  }

  auto* context = reinterpret_cast<WindowContext*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (context != nullptr && context->monitoring.load(std::memory_order_acquire)) {
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
        message == WM_KEYUP || message == WM_SYSKEYUP) {
      const bool is_break = message == WM_KEYUP || message == WM_SYSKEYUP;
      const bool is_repeat =
          !is_break && (static_cast<std::uintptr_t>(lparam) & (1ULL << 30)) != 0;
      context->state->record_foreground(static_cast<std::uint16_t>(wparam), is_break,
                                        is_repeat, qpc_now());
    } else if (message == WM_KILLFOCUS) {
      context->state->record_focus_loss();
    }
  }

  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    const auto device_context = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    constexpr wchar_t instruction[] =
        L"Physical pass-through probe\n\nPress and release K exactly once.\n"
        L"Do not press modifiers or any other key. Keep this window focused.";
    DrawTextW(device_context, instruction, -1, &client,
              DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    EndPaint(window, &paint);
  }

  return DefWindowProcW(window, message, wparam, lparam);
}

int unavailable(const char* reason) {
  std::cout << "SKIP: " << reason << '\n';
  return kUnavailable;
}

bool observation_is_contaminated(
    const rebelliocap::hardware_test::HotkeyPassthroughSnapshot& snapshot) noexcept {
  return snapshot.focus_lost || snapshot.unrelated_foreground_events != 0 ||
         snapshot.foreground_repeats != 0 || snapshot.foreground_key_downs > 1 ||
         snapshot.foreground_key_ups > 1 || snapshot.raw_input_presses > 1;
}

bool observation_is_complete(
    const rebelliocap::hardware_test::HotkeyPassthroughSnapshot& snapshot) noexcept {
  return snapshot.foreground_key_downs == 1 && snapshot.foreground_key_ups == 1 &&
         snapshot.raw_input_presses == 1;
}

}  // namespace

int main() {
  if (!interactive_probe_enabled()) {
    return unavailable(
        "physical keyboard interaction is required; set "
        "REBELLIOCAP_RUN_INTERACTIVE_HOTKEY_PROBE=1 and run this hardware test "
        "from an interactive local Windows desktop");
  }
  if (GetSystemMetrics(SM_REMOTESESSION) != 0) {
    return unavailable("a remote session cannot prove local physical keyboard pass-through");
  }

  rebelliocap::hardware_test::HotkeyPassthroughState state(kTestVirtualKey);
  WindowContext context{.state = &state};
  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = test_window_proc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassW(&window_class) == 0) {
    return unavailable("could not register the interactive foreground test window");
  }

  const auto window = CreateWindowExW(
      0, kWindowClass, L"RebellioCap physical keyboard pass-through probe",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 560, 280, nullptr, nullptr,
      instance, &context);
  if (window == nullptr) {
    UnregisterClassW(kWindowClass, instance);
    return unavailable("could not create the interactive foreground test window");
  }

  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);
  const auto previous_foreground = GetForegroundWindow();
  const auto foreground_thread = previous_foreground == nullptr
                                     ? 0
                                     : GetWindowThreadProcessId(previous_foreground, nullptr);
  const auto current_thread = GetCurrentThreadId();
  const bool attached = foreground_thread != 0 && foreground_thread != current_thread &&
                        AttachThreadInput(current_thread, foreground_thread, TRUE) != FALSE;
  BringWindowToTop(window);
  SetForegroundWindow(window);
  SetFocus(window);
  if (attached) {
    AttachThreadInput(current_thread, foreground_thread, FALSE);
  }

  const auto focus_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  MSG message{};
  while (GetForegroundWindow() != window && std::chrono::steady_clock::now() < focus_deadline) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (GetForegroundWindow() != window) {
    DestroyWindow(window);
    UnregisterClassW(kWindowClass, instance);
    return unavailable("Windows did not grant foreground focus to the interactive test window");
  }

  rebelliocap::RawInputHotkey observer;
  const rebelliocap::HotkeyBinding binding{
      .action = rebelliocap::HotkeyAction::SaveReplay,
      .chord = {.virtual_key = kTestVirtualKey}};
  const auto started = observer.start(
      std::span(&binding, 1),
      [&](rebelliocap::HotkeyAction, rebelliocap::QpcTicks observed_at) {
        state.record_raw_press(observed_at);
      });
  if (!started.is_success()) {
    std::cerr << "FAIL: Raw Input observer did not start: " << started.error().message << '\n';
    DestroyWindow(window);
    UnregisterClassW(kWindowClass, instance);
    return 1;
  }

  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (GetForegroundWindow() != window) {
    observer.stop();
    DestroyWindow(window);
    UnregisterClassW(kWindowClass, instance);
    return unavailable("foreground focus changed before physical observation began");
  }

  state.begin(qpc_now());
  context.monitoring.store(true, std::memory_order_release);
  std::cout << "INTERACTIVE: press and release K exactly once within 15 seconds; "
               "do not press modifiers or any other key, and keep the probe window focused\n";

  const auto interaction_deadline = std::chrono::steady_clock::now() + kInteractionTimeout;
  std::optional<std::chrono::steady_clock::time_point> stability_deadline;
  while (std::chrono::steady_clock::now() < interaction_deadline) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (GetForegroundWindow() != window) {
      state.record_focus_loss();
    }

    const auto snapshot = state.snapshot();
    if (observation_is_contaminated(snapshot)) {
      break;
    }
    if (observation_is_complete(snapshot)) {
      if (!stability_deadline.has_value()) {
        stability_deadline = std::chrono::steady_clock::now() + kStabilityInterval;
      } else if (std::chrono::steady_clock::now() >= *stability_deadline) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  context.monitoring.store(false, std::memory_order_release);
  state.finish(qpc_now());
  observer.stop();
  DestroyWindow(window);
  UnregisterClassW(kWindowClass, instance);

  const auto evaluation = state.evaluate();
  const auto& snapshot = evaluation.snapshot;
  std::cout << "foreground_keydown=" << snapshot.foreground_key_downs
            << " foreground_keyup=" << snapshot.foreground_key_ups
            << " foreground_repeats=" << snapshot.foreground_repeats
            << " unrelated_foreground_events=" << snapshot.unrelated_foreground_events
            << " raw_input_callback=" << snapshot.raw_input_presses
            << " focus_lost=" << snapshot.focus_lost
            << " foreground_down_qpc=" << snapshot.foreground_down_qpc
            << " foreground_up_qpc=" << snapshot.foreground_up_qpc
            << " observer_qpc=" << snapshot.raw_input_qpc
            << " interval_qpc=[" << snapshot.observation_started << ','
            << snapshot.observation_finished << "]\n";
  if (!evaluation.passed) {
    std::cerr << "FAIL: " << evaluation.reason << '\n';
    return 1;
  }

  std::cout << "PASS: one physical K make/break reached the normal foreground message path "
               "and the passive Raw Input observer accepted exactly one press\n";
  return 0;
}
