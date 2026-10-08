#include "audio/wasapi_capture_source.h"
#include "audio/wasapi_pcm.h"
#include "audio/wasapi_timeline.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <exception>
#include <limits>

namespace rebelliocap {
using Microsoft::WRL::ComPtr;
namespace {
Error error(HRESULT hr, const char* action) {
  const char* code = "audio.capture_failed";
  if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) code = "audio.endpoint_lost";
  else if (hr == E_NOTFOUND || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) code = "audio.endpoint_unavailable";
  else if (hr == E_ACCESSDENIED) code = "audio.access_denied";
  return {code, action, hr};
}
// Callback owns its event until the final COM Release; concurrent callbacks never
// access source state, close its handles, or activate endpoints.
class Notifications final : public IMMNotificationClient {
 public:
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) { *out = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { const auto count = --refs_; if (!count) delete this; return count; }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { SetEvent(event); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { SetEvent(event); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { SetEvent(event); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { SetEvent(event); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
 private:
  ~Notifications() { if (event) CloseHandle(event); }
  std::atomic<ULONG> refs_{1};
};
}
struct WasapiCaptureSource::Impl {
  AudioEndpointSelection selection;
  QpcTicks frequency;
  DWORD owner = GetCurrentThreadId();
  bool com = false, registered = false, started = false;
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<Notifications> notifications;
  ComPtr<IMMDevice> device;
  ComPtr<IAudioClient3> client;
  ComPtr<IAudioCaptureClient> capture;
  WAVEFORMATEX* format = nullptr;
  HANDLE ready = nullptr;
  std::wstring endpoint;
  std::optional<Error> terminal;
  audio_detail::WasapiTimeline timeline;
  Impl(AudioEndpointSelection s, QpcTicks f) : selection(std::move(s)), frequency(f), timeline(f) {}
  void close_stream() {
    if (started) client->Stop();
    started = false;
    capture.Reset(); client.Reset(); device.Reset();
    if (format) CoTaskMemFree(format);
    format = nullptr;
    if (ready) CloseHandle(ready);
    ready = nullptr;
  }
  ~Impl() {
    // Destruction on another thread cannot safely release this COM apartment.
    if (GetCurrentThreadId() != owner) std::terminate();
    if (registered) enumerator->UnregisterEndpointNotificationCallback(notifications.Get());
    close_stream(); notifications.Reset(); enumerator.Reset();
    if (com) CoUninitialize();
  }
  HRESULT selected(ComPtr<IMMDevice>& target) {
    return enumerator->GetDevice(selection.endpoint_id->c_str(), &target);
  }
  Result<void> open() {
    close_stream();
    timeline = audio_detail::WasapiTimeline(frequency);
    HRESULT hr = selected(device);
    if (FAILED(hr)) return Result<void>::failure(error(hr, "Select audio endpoint"));
    ComPtr<IMMEndpoint> endpoint_info;
    hr = device.As(&endpoint_info);
    if (FAILED(hr)) return Result<void>::failure(error(hr, "Query endpoint direction"));
    EDataFlow flow;
    hr = endpoint_info->GetDataFlow(&flow);
    if (FAILED(hr)) return Result<void>::failure(error(hr, "Get endpoint direction"));
    if (flow != (selection.role == AudioEndpointRole::SystemLoopback ? eRender : eCapture)) return Result<void>::failure({"audio.endpoint_wrong_role", "Pinned endpoint has wrong direction", {}});
    LPWSTR id = nullptr;
    hr = device->GetId(&id);
    if (FAILED(hr)) return Result<void>::failure(error(hr, "Get endpoint id"));
    endpoint = id; CoTaskMemFree(id);
    hr = device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return Result<void>::failure(error(hr, "Activate IAudioClient3"));
    hr = client->GetMixFormat(&format);
    if (FAILED(hr)) return Result<void>::failure(error(hr, "Get endpoint mix format"));
    auto valid = audio_detail::decode_packet(*format, nullptr, 0, AUDCLNT_BUFFERFLAGS_SILENT, 0, frequency, StreamKind::SystemAudio);
    if (!valid.is_success()) return Result<void>::failure(valid.error());
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (selection.role == AudioEndpointRole::SystemLoopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    // Shared-mode loopback requires inherited Initialize, not InitializeSharedAudioStream.
    // A bounded 100ms engine buffer tolerates worker scheduling without a PCM queue.
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, format, nullptr);
    if (FAILED(hr)) return Result<void>::failure(error(hr, ("Initialize shared event stream rate=" + std::to_string(format->nSamplesPerSec) + " channels=" + std::to_string(format->nChannels) + " tag=" + std::to_string(format->wFormatTag) + " bits=" + std::to_string(format->wBitsPerSample)).c_str()));
    ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready) return Result<void>::failure(error(HRESULT_FROM_WIN32(GetLastError()), "Create audio event"));
    hr = client->SetEventHandle(ready);
    if (FAILED(hr)) return Result<void>::failure(error(hr, "Set audio event"));
    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) return Result<void>::failure(error(hr, "Get capture service"));
    hr = client->Start();
    if (FAILED(hr)) return Result<void>::failure(error(hr, "Start capture"));
    started = true;
    return Result<void>::success();
  }
  Result<std::optional<PcmBlock>> lost(Error reason) {
    close_stream();
    terminal = reason;
    return Result<std::optional<PcmBlock>>::failure(std::move(reason));
  }
};
WasapiCaptureSource::WasapiCaptureSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WasapiCaptureSource::~WasapiCaptureSource() = default;
Result<std::unique_ptr<WasapiCaptureSource>> WasapiCaptureSource::create(AudioEndpointSelection selection, QpcClock& clock) {
  using R = Result<std::unique_ptr<WasapiCaptureSource>>;
  if (!selection.endpoint_id.has_value() || selection.endpoint_id->empty()) {
    return R::failure({"audio.endpoint_required",
                       "A pinned stable audio endpoint ID is required.", {}});
  }
  auto impl = std::make_unique<Impl>(std::move(selection), clock.frequency());
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) return R::failure(error(hr, "Capture requires owning MTA worker"));
  impl->com = true;
  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&impl->enumerator));
  if (FAILED(hr)) return R::failure(error(hr, "Create endpoint enumerator"));
  impl->notifications.Attach(new Notifications());
  if (!impl->notifications->event) return R::failure(error(HRESULT_FROM_WIN32(GetLastError()), "Create endpoint event"));
  hr = impl->enumerator->RegisterEndpointNotificationCallback(impl->notifications.Get());
  if (FAILED(hr)) return R::failure(error(hr, "Register endpoint notifications"));
  impl->registered = true;
  auto opened = impl->open();
  if (!opened.is_success()) return R::failure(opened.error());
  return R::success(std::unique_ptr<WasapiCaptureSource>(new WasapiCaptureSource(std::move(impl))));
}
Result<std::optional<PcmBlock>> WasapiCaptureSource::next_block(std::chrono::milliseconds timeout) {
  using R = Result<std::optional<PcmBlock>>;
  auto& s = *impl_;
  if (GetCurrentThreadId() != s.owner) return R::failure({"audio.wrong_thread", "Use source on its owning capture worker", {}});
  if (timeout.count() < 0 || timeout.count() > MAXDWORD - 1LL) return R::failure({"audio.timeout_invalid", "Timeout outside finite DWORD range", {}});
  if (s.terminal) return R::failure(*s.terminal);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (WaitForSingleObject(s.notifications->event, 0) == WAIT_OBJECT_0) {
      ComPtr<IMMDevice> selected;
      auto hr = s.selected(selected);
      bool changed = FAILED(hr);
      if (SUCCEEDED(hr)) {
        LPWSTR id = nullptr;
        hr = selected->GetId(&id);
        if (FAILED(hr)) return R::failure(error(hr, "Check endpoint id"));
        changed = s.endpoint != id; CoTaskMemFree(id);
        DWORD state = 0;
        hr = selected->GetState(&state);
        changed = changed || FAILED(hr) || state != DEVICE_STATE_ACTIVE;
      }
      if (changed) return s.lost({"audio.endpoint_lost", "Selected endpoint changed or became unavailable", {}});
    }
    UINT32 frames = 0;
    HRESULT hr = s.capture->GetNextPacketSize(&frames);
    if (FAILED(hr)) { auto e = error(hr, "Get next audio packet"); if (e.code == "audio.endpoint_lost") return s.lost(std::move(e)); return R::failure(std::move(e)); }
    if (frames) {
      BYTE* data = nullptr; DWORD flags = 0; UINT64 device_position = 0, timestamp = 0;
      hr = s.capture->GetBuffer(&data, &frames, &flags, &device_position, &timestamp);
      if (FAILED(hr)) { auto e = error(hr, "Get audio buffer"); if (e.code == "audio.endpoint_lost") return s.lost(std::move(e)); return R::failure(std::move(e)); }
      // BUFFER_EMPTY acquired no packet; GetNextPacketSize is only a snapshot.
      if (hr == AUDCLNT_S_BUFFER_EMPTY) return R::success(std::nullopt);
      struct Release { IAudioCaptureClient* capture; UINT32 frames; ~Release() { if (capture) capture->ReleaseBuffer(frames); } } release{s.capture.Get(), frames};
      auto block = audio_detail::decode_packet(*s.format, data, frames, flags, timestamp, s.frequency, s.selection.role == AudioEndpointRole::SystemLoopback ? StreamKind::SystemAudio : StreamKind::MicrophoneAudio);
      hr = s.capture->ReleaseBuffer(frames); release.capture = nullptr;
      if (FAILED(hr)) { auto e = error(hr, "Release audio buffer"); if (e.code == "audio.endpoint_lost") return s.lost(std::move(e)); return R::failure(std::move(e)); }
      if (!block.is_success()) return R::failure(block.error());
      auto timing = s.timeline.stamp(block.value(), device_position, frames);
      if (!timing.is_success() && timing.error().code == "audio.epoch_required") {
        s.timeline = audio_detail::WasapiTimeline(s.frequency);
        block.value().discontinuity = true;
        timing = s.timeline.stamp(block.value(), device_position, frames);
      }
      if (!timing.is_success()) return R::failure(timing.error());
      return R::success(std::move(block).value());
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return R::success(std::nullopt);
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    HANDLE events[] = {s.notifications->event, s.ready};
    const auto wait = WaitForMultipleObjects(2, events, FALSE, static_cast<DWORD>(remaining));
    if (wait == WAIT_TIMEOUT) return R::success(std::nullopt);
    if (wait == WAIT_FAILED) return R::failure(error(HRESULT_FROM_WIN32(GetLastError()), "Wait for audio"));
    // The auto-reset notification was consumed by WaitForMultipleObjects.
    if (wait == WAIT_OBJECT_0) SetEvent(s.notifications->event);
  }
}
}
