#include <Windows.h>
#include <dwmapi.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>
#include <string_view>

#include <nlohmann/json.hpp>

#include "capture/monitor_catalog.h"
#include "engine/hardware_pipeline_factory.h"
#include "platform/windows/com_apartment.h"

// A 60 FPS bitstream can contain only 30 fresh desktop images. Exercise the
// production pipeline against an independently updating desktop window instead
// of treating packet count as proof of capture cadence. Requires an interactive
// desktop, NVENC, and a display/stimulus that can sustain at least 60 Hz.
int main(int argc, char** argv) {
  using namespace rebelliocap;
  using namespace std::chrono;
  const ComApartment apartment;
  if (!apartment.initialized()) return 77;
  auto monitors = enumerate_monitors();
  if (!monitors.is_success() || monitors.value().empty()) return 77;
  auto monitor = monitors.value().front();
  for (const auto& candidate : monitors.value()) {
    if (candidate.primary) monitor = candidate;
  }
  EngineConfig config;
  config.recording_priority=argc>1 && std::string_view(argv[1])=="--recording-priority";
  config.monitor = monitor.id;
  config.system_audio_enabled = false;
  config.microphone_enabled = false;
  HardwarePipelineFactory factory;
  auto dependencies = factory.create(config);
  if (!dependencies.is_success()) {
    std::cerr << "SKIP: " << dependencies.error().code << '\n';
    return 77;
  }
  auto pipeline = dependencies.value().video;
  std::atomic<unsigned> presents{0};
  std::promise<bool> ready;
  auto ready_future = ready.get_future();
  std::jthread stimulus([&](std::stop_token stop) {
    HWND window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"RebellioCap capture cadence test", WS_POPUP | WS_VISIBLE,
        monitor.desktop_rect.left + 20, monitor.desktop_rect.top + 100,
        320, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) { ready.set_value(false); return; }
    HDC dc = GetDC(window);
    if (!dc) { DestroyWindow(window); ready.set_value(false); return; }
    ready.set_value(true);
    while (!stop.stop_requested()) {
      const unsigned frame = presents.load();
      RECT background{0, 0, 320, 160};
      FillRect(dc, &background, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      const auto x = static_cast<LONG>((frame * 5) % 280);
      RECT bar{x, 0, x + 40, 160};
      FillRect(dc, &bar, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
      GdiFlush();
      if (FAILED(DwmFlush())) break;
      ++presents;
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    ReleaseDC(window, dc);
    DestroyWindow(window);
  });
  if (!ready_future.get()) return 77;
  auto resumed = pipeline->resume();
  if (!resumed.is_success()) {
    std::cerr << resumed.error().code << '\n';
    return 1;
  }
  const auto begin = steady_clock::now();
  const auto origin = dependencies.value().clock->now();
  const auto frequency = dependencies.value().clock->frequency();
  const auto first_present = presents.load();
  constexpr unsigned ticks = 360;
  std::vector<QpcTicks> presentation;
  for (unsigned tick = 0; tick < ticks; ++tick) {
    const auto scheduled = origin + tick * frequency / 60;
    dependencies.value().clock->wait_until(scheduled, {});
    auto packets = pipeline->tick(scheduled, tick == 0);
    if (!packets.is_success()) {
      std::cerr << packets.error().code << '\n';
      return 1;
    }
    for (const auto& packet : packets.value()) presentation.push_back(packet.pts);
  }
  const double seconds = duration<double>(steady_clock::now() - begin).count();
  const auto present_count = presents.load() - first_present;
  stimulus.request_stop();
  stimulus.join();
  auto flushed = pipeline->flush();
  const auto metrics = pipeline->metrics();
  pipeline->suspend();
  if (!flushed.is_success()) {
    std::cerr << flushed.error().code << '\n';
    return 1;
  }
  for (const auto& packet : flushed.value()) presentation.push_back(packet.pts);
  std::sort(presentation.begin(), presentation.end());
  std::size_t off_grid = 0;
  for (const auto pts : presentation) {
    const auto elapsed = pts - origin;
    const auto index = (elapsed * 60 + frequency - 1) / frequency;
    const auto expected = origin + index * frequency / 60;
    if (pts != expected) ++off_grid;
  }
  std::cout << nlohmann::json{
      {"recording_priority_requested",metrics.recording_priority_requested},
      {"recording_priority_applied",metrics.recording_priority_applied},
      {"recording_gpu_priority",metrics.recording_gpu_priority},
      {"seconds", seconds}, {"ticks", ticks},
      {"stimulus_fps", present_count / seconds},
      {"fresh_image_fps", metrics.new_image_ticks / seconds},
      {"new_images", metrics.new_image_ticks},
      {"repeated_images", metrics.repeated_image_ticks},
      {"submitted_frames", metrics.submitted_frames},
      {"completed_frames", metrics.completed_frames},
      {"off_grid_presentation_timestamps", off_grid}}.dump() << '\n';
  if (present_count / seconds < 58) {
    std::cerr << "SKIP: desktop stimulus did not sustain 60 Hz\n";
    return 77;
  }
  // Allow ordinary scheduling/compositor jitter, but reject half-rate capture.
  return metrics.new_image_ticks >= ticks * 85 / 100 &&
         metrics.submitted_frames >= ticks * 85 / 100 &&
         metrics.completed_frames == metrics.submitted_frames && off_grid == 0 ? 0 : 1;
}
