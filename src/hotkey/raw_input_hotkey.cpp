#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "hotkey/raw_input_hotkey.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rebelliocap {
namespace {

constexpr wchar_t kWindowClassName[] = L"RebellioCapRawInputHotkey";
constexpr UINT kDispatchQueuedEvent = WM_APP + 0x532U;
constexpr int kRegisteredHotkeyFirstId = 0x5343;
constexpr int kRegisteredHotkeyLastId = 0x53FF;

std::mutex registrar_mutex;
bool registrar_active = false;

Error hotkey_error(std::string code, std::string message,
                   std::optional<long> system_error = std::nullopt) {
  return {.code = std::move(code),
          .message = std::move(message),
          .hresult = system_error};
}

bool acquire_registrar() {
  std::lock_guard lock(registrar_mutex);
  if (registrar_active) {
    return false;
  }
  registrar_active = true;
  return true;
}

void release_registrar() noexcept {
  std::lock_guard lock(registrar_mutex);
  registrar_active = false;
}

class RegistrarRelease final {
 public:
  ~RegistrarRelease() { release_registrar(); }
};

std::uint16_t normalized_virtual_key(RawKeyboardEvent event) noexcept {
  switch (event.virtual_key) {
    case VK_CONTROL:
      return event.e0 ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
      return event.e0 ? VK_RMENU : VK_LMENU;
    case VK_SHIFT:
      return event.make_code == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    default:
      return event.virtual_key;
  }
}

std::uint8_t modifier_bit(std::uint16_t virtual_key) noexcept {
  switch (virtual_key) {
    case VK_LCONTROL:
      return 1U << 0U;
    case VK_RCONTROL:
      return 1U << 1U;
    case VK_LMENU:
      return 1U << 2U;
    case VK_RMENU:
      return 1U << 3U;
    case VK_LSHIFT:
      return 1U << 4U;
    case VK_RSHIFT:
      return 1U << 5U;
    default:
      return 0;
  }
}

UINT registered_hotkey_modifiers(HotkeyChord chord) noexcept {
  UINT modifiers = MOD_NOREPEAT;
  if (chord.control) modifiers |= MOD_CONTROL;
  if (chord.alt) modifiers |= MOD_ALT;
  if (chord.shift) modifiers |= MOD_SHIFT;
  return modifiers;
}

QpcTicks message_arrival_qpc(const QpcClock& clock) noexcept {
  const auto processed_at = clock.now();
  const auto message_time = static_cast<DWORD>(GetMessageTime());
  const auto elapsed_milliseconds =
      static_cast<QpcTicks>(GetTickCount() - message_time);
  const auto elapsed_ticks =
      clock.frequency() * elapsed_milliseconds / 1'000;
  return processed_at >= elapsed_ticks ? processed_at - elapsed_ticks : 0;
}

}  // namespace

HotkeyChordMatcher::HotkeyChordMatcher(HotkeyChord chord) noexcept : chord_(chord) {}

bool HotkeyChordMatcher::accept(RawKeyboardEvent event) noexcept {
  try {
    auto& state = devices_[event.device];
    const auto virtual_key = normalized_virtual_key(event);
    const auto bit = modifier_bit(virtual_key);
    if (bit != 0) {
      if (event.is_break) {
        state.modifiers &= static_cast<std::uint8_t>(~bit);
      } else {
        state.modifiers |= bit;
      }
    }

    if (virtual_key != chord_.virtual_key) {
      return false;
    }
    if (event.is_break) {
      state.armed = true;
      return false;
    }
    if (!state.armed) {
      return false;
    }
    state.armed = false;

    std::uint8_t logical_modifiers = 0;
    for (const auto& entry : devices_) {
      logical_modifiers |= entry.second.modifiers;
    }
    const bool control_down = (logical_modifiers & (LeftControl | RightControl)) != 0;
    const bool alt_down = (logical_modifiers & (LeftAlt | RightAlt)) != 0;
    const bool shift_down = (logical_modifiers & (LeftShift | RightShift)) != 0;
    if (control_down != chord_.control || alt_down != chord_.alt ||
        shift_down != chord_.shift) {
      return false;
    }

    return true;
  } catch (const std::exception&) {
    return false;
  }
}

void HotkeyChordMatcher::remove_device(std::uintptr_t device) noexcept { devices_.erase(device); }

void HotkeyChordMatcher::reset() noexcept { devices_.clear(); }

struct RawInputHotkey::Impl {
  enum class CallbackBackend { RawInput, RegisteredHotkey };

  struct QueuedEvent {
    RawKeyboardEvent event;
    QpcTicks timestamp;
  };

  struct BindingMatcher {
    explicit BindingMatcher(HotkeyBinding value)
        : binding(value), matcher(value.chord) {}

    HotkeyBinding binding;
    HotkeyChordMatcher matcher;
  };

  struct RegisteredBinding {
    int id;
    HotkeyAction action;
  };

  struct CallbackObservation {
    HotkeyAction action;
    CallbackBackend backend;
    QpcTicks timestamp;
  };

  struct WorkerState {
    WorkerState(std::span<const HotkeyBinding> bindings, HotkeyCallback callback)
        : on_press(std::move(callback)) {
      matchers.reserve(bindings.size());
      registered_bindings.reserve(bindings.size());
      callback_observations.reserve(2);
      for (const auto binding : bindings) {
        matchers.emplace_back(binding);
      }
    }

    void publish_initialization(std::optional<Error> error) {
      {
        std::lock_guard lock(initialization_mutex);
        initialization_error = std::move(error);
        initialized = true;
      }
      initialization_cv.notify_one();
    }

    void request_stop() noexcept {
      if (stopping.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      const auto native_window = window.load(std::memory_order_acquire);
      if (native_window != nullptr) {
        PostMessageW(native_window, WM_CLOSE, 0, 0);
        return;
      }
      const auto id = message_thread_id.load(std::memory_order_acquire);
      if (id != 0) {
        PostThreadMessageW(id, WM_QUIT, 0, 0);
      }
    }

    bool post_test_event(RawKeyboardEvent event, QpcTicks timestamp) {
      return post_test_events({{event, timestamp}});
    }

    bool post_test_events(
        const std::vector<std::pair<RawKeyboardEvent, QpcTicks>>& events) {
      const auto native_window = window.load(std::memory_order_acquire);
      if (native_window == nullptr || stopping.load(std::memory_order_acquire)) {
        return false;
      }
      try {
        std::lock_guard lock(queued_events_mutex);
        for (const auto& [event, timestamp] : events) {
          queued_events.push_back({.event = event, .timestamp = timestamp});
        }
      } catch (const std::exception&) {
        return false;
      }
      return PostMessageW(native_window, kDispatchQueuedEvent, 0, 0) != FALSE;
    }

    std::optional<std::uintptr_t> last_extra_information() const {
      std::lock_guard lock(observation_mutex);
      return observed_extra_information;
    }

    std::optional<Error> failure() const {
      std::lock_guard lock(failure_mutex);
      return callback_failure.has_value() ? callback_failure : backend_failure;
    }

    void record_callback_failure(std::string message) noexcept {
      try {
        std::lock_guard lock(failure_mutex);
        callback_failure = hotkey_error("hotkey.callback_exception", std::move(message));
      } catch (const std::exception&) {
        std::lock_guard lock(failure_mutex);
        callback_failure = Error{.code = "hotkey.callback_exception",
                                 .message = "The hotkey callback threw an exception.",
                                 .hresult = std::nullopt};
      }
    }

    void record_backend_failure(Error error) noexcept {
      try {
        std::lock_guard lock(failure_mutex);
        backend_failure = std::move(error);
      } catch (const std::exception&) {
        std::lock_guard lock(failure_mutex);
        backend_failure = Error{
            .code = "hotkey.fallback_registration_failed",
            .message = "A Windows hotkey fallback could not be registered.",
            .hresult = std::nullopt};
      }
    }

    void invoke_callback(HotkeyAction action, QpcTicks timestamp,
                         CallbackBackend backend) noexcept {
      if (stopping.load(std::memory_order_acquire)) {
        return;
      }
      {
        std::lock_guard lock(callback_mutex);
        if (stopping.load(std::memory_order_acquire)) {
          return;
        }
        auto previous = callback_observations.end();
        for (auto observation = callback_observations.begin();
             observation != callback_observations.end(); ++observation) {
          if (observation->action == action) {
            previous = observation;
            break;
          }
        }
        bool duplicate = false;
        if (previous != callback_observations.end() && previous->backend != backend) {
          const auto delta = timestamp >= previous->timestamp
                                 ? timestamp - previous->timestamp
                                 : previous->timestamp - timestamp;
          duplicate = delta <= clock.frequency() / 10;
        }
        if (duplicate) {
          return;
        }
        if (previous == callback_observations.end()) {
          callback_observations.push_back(
              {.action = action, .backend = backend, .timestamp = timestamp});
        } else {
          previous->backend = backend;
          previous->timestamp = timestamp;
        }
      }
      if (stopping.load(std::memory_order_acquire)) {
        return;
      }
      try {
        auto callback = on_press;
        callback(action, timestamp);
      } catch (const std::exception& error) {
        record_callback_failure(std::string("The hotkey callback threw: ") + error.what());
        request_stop();
      } catch (...) {
        record_callback_failure("The hotkey callback threw a non-standard exception.");
        request_stop();
      }
    }

    void dispatch(RawKeyboardEvent event, QpcTicks timestamp) noexcept {
      if (stopping.load(std::memory_order_acquire)) {
        return;
      }
      for (auto& binding_matcher : matchers) {
        if (!binding_matcher.matcher.accept(event)) {
          continue;
        }
        if (stopping.load(std::memory_order_acquire)) {
          return;
        }
        {
          std::lock_guard lock(observation_mutex);
          observed_extra_information = event.extra_information;
        }
        invoke_callback(binding_matcher.binding.action, timestamp,
                        CallbackBackend::RawInput);
      }
    }

    void dispatch_queued_events() noexcept {
      for (;;) {
        if (stopping.load(std::memory_order_acquire)) {
          return;
        }
        std::optional<QueuedEvent> queued;
        {
          std::lock_guard lock(queued_events_mutex);
          if (queued_events.empty()) {
            return;
          }
          queued = queued_events.front();
          queued_events.pop_front();
        }
        dispatch(queued->event, queued->timestamp);
      }
    }

    void handle_raw_input(HRAWINPUT input_handle, QpcTicks timestamp) noexcept {
      if (stopping.load(std::memory_order_acquire)) {
        return;
      }
      try {
        UINT bytes = 0;
        if (GetRawInputData(input_handle, RID_INPUT, nullptr, &bytes,
                            sizeof(RAWINPUTHEADER)) != 0 ||
            bytes < sizeof(RAWINPUT)) {
          return;
        }

        std::vector<std::byte> storage(bytes);
        if (GetRawInputData(input_handle, RID_INPUT, storage.data(), &bytes,
                            sizeof(RAWINPUTHEADER)) != bytes) {
          return;
        }
        const auto* input = reinterpret_cast<const RAWINPUT*>(storage.data());
        if (input->header.dwType != RIM_TYPEKEYBOARD || input->data.keyboard.VKey == 0xFF) {
          return;
        }

        const auto flags = input->data.keyboard.Flags;
        dispatch({.virtual_key = input->data.keyboard.VKey,
                  .make_code = input->data.keyboard.MakeCode,
                  .e0 = (flags & RI_KEY_E0) != 0,
                  .is_break = (flags & RI_KEY_BREAK) != 0,
                  .device = reinterpret_cast<std::uintptr_t>(input->header.hDevice),
                  .extra_information = input->data.keyboard.ExtraInformation},
                 timestamp);
      } catch (const std::exception&) {
        record_callback_failure("The Raw Input message could not be processed.");
        request_stop();
      }
    }

    static LRESULT CALLBACK window_proc(HWND native_window, UINT message, WPARAM wparam,
                                        LPARAM lparam) noexcept {
      auto* self = reinterpret_cast<WorkerState*>(
          GetWindowLongPtrW(native_window, GWLP_USERDATA));
      if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<WorkerState*>(create->lpCreateParams);
        SetWindowLongPtrW(native_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      }

      if (self != nullptr && message == WM_INPUT) {
        self->handle_raw_input(reinterpret_cast<HRAWINPUT>(lparam),
                               message_arrival_qpc(self->clock));
        return DefWindowProcW(native_window, message, wparam, lparam);
      }
      if (self != nullptr && message == WM_INPUT_DEVICE_CHANGE && wparam == GIDC_REMOVAL) {
        for (auto& binding_matcher : self->matchers) {
          binding_matcher.matcher.remove_device(static_cast<std::uintptr_t>(lparam));
        }
        return DefWindowProcW(native_window, message, wparam, lparam);
      }
      if (self != nullptr && message == WM_HOTKEY) {
        for (const auto registered : self->registered_bindings) {
          if (wparam == static_cast<WPARAM>(registered.id)) {
            self->invoke_callback(registered.action,
                                  message_arrival_qpc(self->clock),
                                  CallbackBackend::RegisteredHotkey);
            return 0;
          }
        }
      }
      if (self != nullptr && message == kDispatchQueuedEvent) {
        self->dispatch_queued_events();
        return 0;
      }
      if (message == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
      }
      if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
      }
      return DefWindowProcW(native_window, message, wparam, lparam);
    }

    void thread_main() noexcept {
      RegistrarRelease registrar_release;
      message_thread_id.store(GetCurrentThreadId(), std::memory_order_release);
      const auto instance = GetModuleHandleW(nullptr);
      WNDCLASSEXW window_class{};
      window_class.cbSize = sizeof(window_class);
      window_class.lpfnWndProc = window_proc;
      window_class.hInstance = instance;
      window_class.lpszClassName = kWindowClassName;

      if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        const auto system_error = static_cast<long>(GetLastError());
        publish_initialization(hotkey_error("hotkey.window_class_failed",
                                            "Could not register the Raw Input window class.",
                                            system_error));
        message_thread_id.store(0, std::memory_order_release);
        return;
      }

      const auto native_window = CreateWindowExW(0, kWindowClassName, L"", 0, 0, 0, 0, 0,
                                                 HWND_MESSAGE, nullptr, instance, this);
      if (native_window == nullptr) {
        const auto system_error = static_cast<long>(GetLastError());
        publish_initialization(hotkey_error("hotkey.window_failed",
                                            "Could not create the Raw Input message window.",
                                            system_error));
        UnregisterClassW(kWindowClassName, instance);
        message_thread_id.store(0, std::memory_order_release);
        return;
      }

      RAWINPUTDEVICE keyboard{};
      keyboard.usUsagePage = 0x01;
      keyboard.usUsage = 0x06;
      keyboard.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
      keyboard.hwndTarget = native_window;
      if (RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard)) == FALSE) {
        const auto system_error = static_cast<long>(GetLastError());
        DestroyWindow(native_window);
        UnregisterClassW(kWindowClassName, instance);
        publish_initialization(hotkey_error("hotkey.registration_failed",
                                            "Could not register the passive Raw Input observer.",
                                            system_error));
        message_thread_id.store(0, std::memory_order_release);
        return;
      }

      for (std::size_t index = 0; index < matchers.size(); ++index) {
        const auto id = kRegisteredHotkeyFirstId + static_cast<int>(index);
        const auto binding = matchers[index].binding;
        if (RegisterHotKey(native_window, id,
                           registered_hotkey_modifiers(binding.chord),
                           binding.chord.virtual_key) != FALSE) {
          registered_bindings.push_back({.id = id, .action = binding.action});
        } else {
          const auto system_error = static_cast<long>(GetLastError());
          record_backend_failure(hotkey_error(
              "hotkey.fallback_registration_failed",
              "Could not register a Windows hotkey fallback; Raw Input remains active.",
              system_error));
        }
      }

      window.store(native_window, std::memory_order_release);
      publish_initialization(std::nullopt);

      MSG message{};
      while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }

      RAWINPUTDEVICE removal{};
      removal.usUsagePage = 0x01;
      removal.usUsage = 0x06;
      removal.dwFlags = RIDEV_REMOVE;
      removal.hwndTarget = nullptr;
      RegisterRawInputDevices(&removal, 1, sizeof(removal));
      for (const auto registered : registered_bindings) {
        UnregisterHotKey(native_window, registered.id);
      }

      if (IsWindow(native_window) != FALSE) {
        DestroyWindow(native_window);
      }
      window.store(nullptr, std::memory_order_release);
      message_thread_id.store(0, std::memory_order_release);
      UnregisterClassW(kWindowClassName, instance);
    }

    std::atomic<HWND> window{nullptr};
    std::atomic<DWORD> message_thread_id{0};
    std::atomic_bool stopping{false};

    std::mutex initialization_mutex;
    std::condition_variable initialization_cv;
    bool initialized{false};
    std::optional<Error> initialization_error;

    mutable std::mutex failure_mutex;
    std::optional<Error> callback_failure;
    std::optional<Error> backend_failure;

    mutable std::mutex observation_mutex;
    std::optional<std::uintptr_t> observed_extra_information;

    std::mutex callback_mutex;
    std::vector<CallbackObservation> callback_observations;

    std::mutex queued_events_mutex;
    std::deque<QueuedEvent> queued_events;

    std::vector<BindingMatcher> matchers;
    std::vector<RegisteredBinding> registered_bindings;
    HotkeyCallback on_press;
    QpcClock clock;
  };

  Result<void> start(std::span<const HotkeyBinding> bindings, HotkeyCallback callback) {
    if (bindings.empty()) {
      return Result<void>::failure(
          hotkey_error("hotkey.missing_binding", "At least one hotkey binding is required."));
    }
    constexpr auto kMaximumBindings =
        static_cast<std::size_t>(kRegisteredHotkeyLastId - kRegisteredHotkeyFirstId + 1);
    if (bindings.size() > kMaximumBindings) {
      return Result<void>::failure(hotkey_error(
          "hotkey.too_many_bindings", "Too many hotkeys were requested for the reserved ID range."));
    }
    for (std::size_t index = 0; index < bindings.size(); ++index) {
      const auto validation = validate_hotkey_chord(bindings[index].chord);
      if (!validation.is_success()) {
        return validation;
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (bindings[previous].chord == bindings[index].chord) {
          return Result<void>::failure(hotkey_error(
              "hotkey.duplicate_chord", "Each configured hotkey chord must be unique."));
        }
      }
    }
    if (!callback) {
      return Result<void>::failure(
          hotkey_error("hotkey.invalid_callback", "A hotkey callback is required."));
    }

    std::unique_lock lifecycle_lock(lifecycle_mutex);
    if (thread.joinable()) {
      return Result<void>::failure(
          hotkey_error("hotkey.already_started", "The Raw Input hotkey observer is running."));
    }
    if (!acquire_registrar()) {
      return Result<void>::failure(hotkey_error(
          "hotkey.registration_in_use", "Another Raw Input keyboard observer is active."));
    }

    std::shared_ptr<WorkerState> new_state;
    try {
      new_state = std::make_shared<WorkerState>(bindings, std::move(callback));
      state = new_state;
      thread = std::thread([new_state] { new_state->thread_main(); });
    } catch (const std::exception&) {
      state.reset();
      release_registrar();
      return Result<void>::failure(
          hotkey_error("hotkey.thread_failed", "Could not create the Raw Input message thread."));
    }

    std::unique_lock initialization_lock(new_state->initialization_mutex);
    new_state->initialization_cv.wait(initialization_lock,
                                      [&] { return new_state->initialized; });
    if (new_state->initialization_error.has_value()) {
      auto error = std::move(*new_state->initialization_error);
      initialization_lock.unlock();
      lifecycle_lock.unlock();
      thread.join();
      lifecycle_lock.lock();
      state.reset();
      return Result<void>::failure(std::move(error));
    }
    return Result<void>::success();
  }

  void stop() noexcept {
    std::unique_lock lock(lifecycle_mutex);
    if (!thread.joinable()) {
      state.reset();
      return;
    }

    const auto owned_state = state;
    if (owned_state != nullptr) {
      owned_state->request_stop();
    }

    if (std::this_thread::get_id() == thread.get_id()) {
      thread.detach();
      state.reset();
      return;
    }
    std::thread owned_thread = std::move(thread);
    lock.unlock();
    owned_thread.join();
    lock.lock();
    state.reset();
  }

  bool post_test_event(RawKeyboardEvent event, QpcTicks timestamp) {
    std::shared_ptr<WorkerState> owned_state;
    {
      std::lock_guard lock(lifecycle_mutex);
      owned_state = state;
    }
    return owned_state != nullptr && owned_state->post_test_event(event, timestamp);
  }

  bool post_test_events(std::vector<std::pair<RawKeyboardEvent, QpcTicks>> events) {
    std::shared_ptr<WorkerState> owned_state;
    {
      std::lock_guard lock(lifecycle_mutex);
      owned_state = state;
    }
    return owned_state != nullptr && owned_state->post_test_events(events);
  }

  std::optional<std::uintptr_t> last_extra_information() const {
    std::shared_ptr<WorkerState> owned_state;
    {
      std::lock_guard lock(lifecycle_mutex);
      owned_state = state;
    }
    return owned_state == nullptr ? std::nullopt : owned_state->last_extra_information();
  }

  std::optional<Error> failure() const {
    std::shared_ptr<WorkerState> owned_state;
    {
      std::lock_guard lock(lifecycle_mutex);
      owned_state = state;
    }
    return owned_state == nullptr ? std::nullopt : owned_state->failure();
  }

  mutable std::mutex lifecycle_mutex;
  std::thread thread;
  std::shared_ptr<WorkerState> state;
};

RawInputHotkey::RawInputHotkey() : impl_(std::make_unique<Impl>()) {}

RawInputHotkey::~RawInputHotkey() { stop(); }

Result<void> RawInputHotkey::start(std::span<const HotkeyBinding> bindings,
                                   HotkeyCallback on_press) {
  return impl_->start(bindings, std::move(on_press));
}

Result<void> RawInputHotkey::start(std::initializer_list<HotkeyBinding> bindings,
                                   HotkeyCallback on_press) {
  return start(std::span(bindings.begin(), bindings.size()), std::move(on_press));
}

void RawInputHotkey::stop() noexcept { impl_->stop(); }

bool RawInputHotkey::post_test_event(RawKeyboardEvent event, QpcTicks timestamp) {
  return impl_->post_test_event(event, timestamp);
}

bool RawInputHotkey::post_test_events(
    std::vector<std::pair<RawKeyboardEvent, QpcTicks>> events) {
  return impl_->post_test_events(std::move(events));
}

std::optional<std::uintptr_t> RawInputHotkey::last_extra_information_for_test() const {
  return impl_->last_extra_information();
}

std::optional<Error> RawInputHotkey::last_failure() const { return impl_->failure(); }

}  // namespace rebelliocap
