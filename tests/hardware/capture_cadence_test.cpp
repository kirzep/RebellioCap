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
  bool sparse_updates = false;
  for (int index = 1; index < argc; ++index) {
    config.recording_priority |= std::string_view(argv[index]) == "--recording-priority";
    sparse_updates |= std::string_view(argv[index]) == "--sparse-updates";
  }
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
    // Cover the display in sparse mode so unrelated application repaints
    // cannot hide acquisition timeouts and accidentally make this test pass.
    const int width = sparse_updates ? monitor.desktop_rect.right - monitor.desktop_rect.left : 320;
    const int height = sparse_updates ? monitor.desktop_rect.bottom - monitor.desktop_rect.top : 160;
    HWND window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"RebellioCap capture cadence test", WS_POPUP | WS_VISIBLE,
        monitor.desktop_rect.left + (sparse_updates ? 0 : 20),
        monitor.desktop_rect.top + (sparse_updates ? 0 : 100),
        width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) { ready.set_value(false); return; }
    HDC dc = GetDC(window);
    if (!dc) { DestroyWindow(window); ready.set_value(false); return; }
    ready.set_value(true);
    while (!stop.stop_requested()) {
      const unsigned frame = presents.load();
      // Sparse updates exercise acquisition timeouts and cached output. A
      // continuously repainting fixture never tests this common desktop path.
      if (!sparse_updates || frame % 4 == 0) {
        RECT background{0, 0, width, height};
        FillRect(dc, &background, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const auto x = static_cast<LONG>((frame * 5) % (width - 40));
        RECT bar{x, 0, x + 40, height};
        FillRect(dc, &bar, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        GdiFlush();
      }
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
      {"recording_gpu_priority",metrics.recording_gpu_priority_available ? nlohmann::json(metrics.recording_gpu_priority) : nlohmann::json(nullptr)},
      {"recording_priority_mode",gpu_recording_priority_mode_name(metrics.recording_priority_mode)},
      {"sparse_updates", sparse_updates},
      {"seconds", seconds}, {"ticks", ticks},
      {"stimulus_fps", present_count / seconds},
      {"fresh_image_fps", metrics.new_image_ticks / seconds},
      {"new_images", metrics.new_image_ticks},
      {"repeated_images", metrics.repeated_image_ticks},
      {"submitted_frames", metrics.submitted_frames},
      {"completed_frames", metrics.completed_frames},
      {"encoder_queue_dropped_frames", metrics.encoder_queue_dropped_frames},
      {"off_grid_presentation_timestamps", off_grid}}.dump() << '\n';
  if (present_count / seconds < 58) {
    std::cerr << "SKIP: desktop stimulus did not sustain 60 Hz\n";
    return 77;
  }
  if (sparse_updates && metrics.repeated_image_ticks == 0) {
    std::cerr << "SKIP: compositor supplied continuous frames; sparse acquisition timeouts were not exercised\n";
    return 77;
  }
  // A stream around 55 FPS still stutters despite passing the old 85% gate.
  // Allow a few startup/compositor misses, not recurring losses during capture.
  return (sparse_updates ? metrics.repeated_image_ticks > 0 : metrics.new_image_ticks >= ticks * 98 / 100) &&
         metrics.submitted_frames >= ticks * 99 / 100 &&
         metrics.encoder_queue_dropped_frames == 0 &&
         metrics.completed_frames == metrics.submitted_frames && off_grid == 0 ? 0 : 1;
}
