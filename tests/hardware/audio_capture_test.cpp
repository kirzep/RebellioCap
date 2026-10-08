#include "audio/wasapi_capture_source.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <cstdlib>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <syncstream>
#include <functiondiscoverykeys_devpkey.h>

using namespace rebelliocap;
using namespace std::chrono_literals;
std::optional<std::wstring> default_endpoint_id(AudioEndpointRole role) {
  using Microsoft::WRL::ComPtr;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(init)) return std::nullopt;
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IMMDevice> device;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
  if (SUCCEEDED(result)) {
    result = enumerator->GetDefaultAudioEndpoint(
        role == AudioEndpointRole::SystemLoopback ? eRender : eCapture,
        eConsole, &device);
  }
  wchar_t* raw_id = nullptr;
  if (SUCCEEDED(result)) result = device->GetId(&raw_id);
  std::optional<std::wstring> id;
  if (SUCCEEDED(result) && raw_id != nullptr && raw_id[0] != L'\0') id = raw_id;
  CoTaskMemFree(raw_id);
  device.Reset();
  enumerator.Reset();
  CoUninitialize();
  return id;
}
void probe_capture_endpoints() {
  using Microsoft::WRL::ComPtr;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(init)) return;
  {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices);
    UINT count = 0;
    if (SUCCEEDED(hr)) devices->GetCount(&count);
    std::cout << "active_capture_endpoints=" << count << '\n';
    for (UINT i = 0; i < count; ++i) {
      ComPtr<IMMDevice> device;
      if (FAILED(devices->Item(i, &device))) continue;
      ComPtr<IPropertyStore> properties;
      PROPVARIANT name{};
      if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) && SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
        std::wcout << L"capture_name=" << name.pwszVal << L'\n';
      }
      PropVariantClear(&name);
      for (const DWORD flags : {DWORD{0}, DWORD{AUDCLNT_STREAMFLAGS_EVENTCALLBACK}}) {
        ComPtr<IAudioClient3> client;
        hr = device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        WAVEFORMATEX* format = nullptr;
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&format);
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, format, nullptr);
        std::cout << "capture_probe=" << i << " flags=" << std::hex << flags << " hr=" << hr << std::dec << '\n';
        CoTaskMemFree(format);
      }
    }
  }
  CoUninitialize();
}
int main() {
    probe_capture_endpoints();
  DWORD handles_before = 0, handles_after = 0;
  // Warm OS audio/COM caches before checking retained process handles.
  for (int cycle = 0; cycle < 9; ++cycle) {
    std::thread worker([] {
      QpcClock clock;
      for (auto role : {AudioEndpointRole::SystemLoopback, AudioEndpointRole::Microphone}) {
        auto endpoint_id = default_endpoint_id(role);
        if (!endpoint_id.has_value()) continue;
        auto source = WasapiCaptureSource::create({role, *endpoint_id}, clock);
        if (source.is_success()) source.value()->next_block(30ms);
      }
    });
    worker.join();
    if (cycle == 0) GetProcessHandleCount(GetCurrentProcess(), &handles_before);
  }
  GetProcessHandleCount(GetCurrentProcess(), &handles_after);
  std::cout << "handles_after_warmup=" << handles_before << " handles_after_8_cycles=" << handles_after << '\n';
  std::atomic<int> failed{handles_after > handles_before ? 1 : 0}, unavailable{0};
  auto run = [&](AudioEndpointRole role) {
    QpcClock clock;
    const auto endpoint_id = default_endpoint_id(role);
    if (!endpoint_id.has_value()) {
      ++unavailable;
      return;
    }
    {
      using Microsoft::WRL::ComPtr;
      const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      ComPtr<IMMDeviceEnumerator> e; ComPtr<IMMDevice> d; ComPtr<IAudioClient3> c;
      HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
      if (SUCCEEDED(hr)) hr = e->GetDefaultAudioEndpoint(role == AudioEndpointRole::SystemLoopback ? eRender : eCapture, eConsole, &d);
      if (SUCCEEDED(hr)) hr = d->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(c.GetAddressOf()));
      WAVEFORMATEX* f = nullptr;
      if (SUCCEEDED(hr)) hr = c->GetMixFormat(&f);
      if (SUCCEEDED(hr)) {
        std::osyncstream out(std::cout);
        out << "probe role=" << static_cast<int>(role) << " mix_bytes=" << std::hex;
        for (size_t i = 0; i < sizeof(WAVEFORMATEX) + f->cbSize; ++i) out << static_cast<int>(reinterpret_cast<BYTE*>(f)[i]) << ',';
        REFERENCE_TIME def=0, min=0;
        out << " device_period_hr=" << c->GetDevicePeriod(&def, &min) << " default=" << std::dec << def << " min=" << min;
        UINT32 p=0,b=0,l=0,h=0;
        out << " engine_hr=" << std::hex << c->GetSharedModeEnginePeriod(f,&p,&b,&l,&h) << " periods=" << std::dec << p << ',' << b << ',' << l << ',' << h;
        WAVEFORMATEX* closest = nullptr;
        out << " supported_hr=" << std::hex << c->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, f, &closest) << '\n';
        CoTaskMemFree(closest);
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | (role == AudioEndpointRole::SystemLoopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        out << " independent_initialize flags=" << std::hex << flags << " hns=1000000,0 hr=" << c->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, f, nullptr) << '\n';
        CoTaskMemFree(f);
      }
      c.Reset(); d.Reset(); e.Reset(); if (SUCCEEDED(init)) CoUninitialize();
    }
    auto created = WasapiCaptureSource::create({role, *endpoint_id}, clock);
    if (!created.is_success()) {
      if (created.error().code == "audio.endpoint_unavailable") {
        ++unavailable;
        std::cout << "not-run: " << (role == AudioEndpointRole::Microphone ? "microphone" : "render") << " endpoint unavailable\n";
      } else { ++failed; std::cerr << "role=" << static_cast<int>(role) << " " << created.error().code << " " << created.error().message << " hr=" << std::hex << created.error().hresult.value_or(0) << '\n'; }
      return;
    }
    auto source = std::move(created).value();
        if (source->next_block(-1ms).is_success()) ++failed;
    std::thread wrong_thread([&] {
      auto result = source->next_block(0ms);
      if (result.is_success() || result.error().code != "audio.wrong_thread") ++failed;
    });
    wrong_thread.join();
    std::uint64_t blocks = 0, silent = 0, discontinuities = 0, timestamp_errors = 0, reconstructed = 0, total_frames = 0, first_position = 0, expected_position = 0;
    QpcTicks max_correction = 0, max_anchor_drift = 0, anchor_pts = 0;
    std::uint64_t anchor_position = 0;
    QpcTicks previous = -1, min_delta = INT64_MAX, max_delta = 0;
    char seconds_text[16]{};
    const auto length = GetEnvironmentVariableA("REBELLIOCAP_AUDIO_SOAK_SECONDS", seconds_text, sizeof(seconds_text));
    const auto seconds = length && length < sizeof(seconds_text) ? std::atoi(seconds_text) : 300;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until) {
      auto result = source->next_block(100ms);
      if (!result.is_success() && result.error().code == "audio.timestamp_invalid") { if (timestamp_errors < 8) std::osyncstream(std::cout) << "role=" << static_cast<int>(role) << " " << result.error().message << '\n'; ++timestamp_errors; continue; }
      if (!result.is_success()) { ++failed; std::cerr << "role=" << static_cast<int>(role) << " " << result.error().code << " " << result.error().message << " hr=" << std::hex << result.error().hresult.value_or(0) << '\n'; return; }
      if (!result.value()) continue;
      const auto& block = *result.value();
      if (block.pts < previous || !block.sample_rate || !block.channels || block.interleaved.empty() || block.interleaved.size() % block.channels) { ++failed; return; }
      if (previous >= 0) { min_delta = (std::min)(min_delta, block.pts - previous); max_delta = (std::max)(max_delta, block.pts - previous); }
      const auto frames = block.interleaved.size() / block.channels;
      if (!blocks) { first_position = block.device_position; expected_position = first_position; anchor_pts = block.raw_pts; anchor_position = block.device_position; }
      if (block.device_position != expected_position) { ++failed; return; }
      expected_position += frames; total_frames += frames;
      if (block.timestamp_reconstructed) {
        if (reconstructed < 8) std::osyncstream(std::cout) << "reconstructed role=" << static_cast<int>(role) << " raw_qpc=" << block.raw_pts << " output_qpc=" << block.pts << " device_position=" << block.device_position << " frames=" << frames << '\n';
        ++reconstructed;
        max_correction = (std::max)(max_correction, block.pts - block.raw_pts);
      } else {
        const auto expected = anchor_pts + static_cast<QpcTicks>(static_cast<long double>(block.device_position - anchor_position) * clock.frequency() / block.sample_rate);
        const auto residual = block.raw_pts >= expected ? block.raw_pts - expected : expected - block.raw_pts;
        max_anchor_drift = (std::max)(max_anchor_drift, residual);
        anchor_pts = block.raw_pts; anchor_position = block.device_position;
      }
      previous = block.pts;
      ++blocks;
      if (block.silent) { ++silent; for (float sample : block.interleaved) if (sample != 0) { ++failed; return; } }
      discontinuities += block.discontinuity;
    }
    std::cout << "role=" << static_cast<int>(role) << " blocks=" << blocks << " silent=" << silent << " discontinuities=" << discontinuities << " timestamp_errors=" << timestamp_errors << " reconstructed=" << reconstructed << " total_frames=" << total_frames << " max_correction_qpc=" << max_correction << " max_trusted_anchor_residual_qpc=" << max_anchor_drift << " min_qpc_delta=" << min_delta << " max_qpc_delta=" << max_delta << '\n';
        if (!blocks || timestamp_errors || expected_position - first_position != total_frames) ++failed;
    source.reset();
    APTTYPE type; APTTYPEQUALIFIER qualifier;
    const auto apartment = CoGetApartmentType(&type, &qualifier);
    if (apartment != CO_E_NOTINITIALIZED && !(apartment == S_OK && qualifier == APTTYPEQUALIFIER_IMPLICIT_MTA)) {
      ++failed; std::cerr << "audio COM apartment was not balanced\n";
    }
  };
  std::thread system(run, AudioEndpointRole::SystemLoopback);
  std::thread mic(run, AudioEndpointRole::Microphone);
  system.join(); mic.join();
  DWORD handles_after_soak = 0;
  if (!GetProcessHandleCount(GetCurrentProcess(), &handles_after_soak)) ++failed;
  std::cout << "handles_after_soak=" << handles_after_soak << '\n';
  if (handles_after_soak > handles_after) ++failed;
  return failed ? 1 : unavailable ? 77 : 0;
}
