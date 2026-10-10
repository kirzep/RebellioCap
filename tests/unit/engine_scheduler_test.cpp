#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <Windows.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/recorder_engine.h"
#include "fakes/fake_audio_source.h"
#include "fakes/fake_capture_source.h"
#include "fakes/fake_continuous_muxer.h"
#include "fakes/fake_video_encoder.h"

namespace {

using namespace std::chrono_literals;
using namespace rebelliocap;
using namespace rebelliocap::testing;

EngineConfig config();

class BlockingMuxer final : public IClipMuxer {
 public:
  Result<std::filesystem::path> write(const ReplaySnapshot& snapshot,
      const std::vector<StreamDescriptor>&, const std::filesystem::path& path,
      Container) override {
    std::unique_lock lock(mutex_);
    snapshot_ = snapshot;
    path_ = path;
    paths_.push_back(path);
    if (std::filesystem::exists(path)) {
      return Result<std::filesystem::path>::failure(
          {"mux.destination_exists", "Destination already exists", {}});
    }
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return released_; });
    return Result<std::filesystem::path>::success(path);
  }

  bool wait_until_entered() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, 5s, [&] { return entered_; });
  }

  ReplaySnapshot snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
  }

  std::filesystem::path path() const {
    std::scoped_lock lock(mutex_);
    return path_;
  }

  std::vector<std::filesystem::path> paths() const {
    std::scoped_lock lock(mutex_);
    return paths_;
  }

  void release() {
    {
      std::scoped_lock lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_{false};
  bool released_{false};
  ReplaySnapshot snapshot_{};
  std::filesystem::path path_;
  std::vector<std::filesystem::path> paths_;
};

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("RebellioCap.Engine-sequence-" + std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directory(path_);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class LaggingVideoPipeline final : public IEngineVideoPipeline {
 public:
  explicit LaggingVideoPipeline(std::shared_ptr<VirtualEngineClock> clock)
      : clock_(std::move(clock)) {}

  Result<std::vector<EncodedPacket>> tick(QpcTicks pts, bool force_keyframe) override {
    clock_->elapse(6'500);
    return delegate_.tick(pts, force_keyframe);
  }

  Result<std::vector<EncodedPacket>> flush() override { return delegate_.flush(); }
  bool wait_for_frames(std::size_t count) { return delegate_.wait_for_frames(count, 5s); }
  std::vector<QpcTicks> timestamps() const { return delegate_.timestamps(); }
  std::vector<bool> forced_keyframes() const { return delegate_.forced_keyframes(); }

 private:
  std::shared_ptr<VirtualEngineClock> clock_;
  FakeVideoEncoder delegate_;
};

class IdleAudioProbe final : public IEngineAudioPipeline {
 public:
  std::uint64_t mixing_dropped_frames() const noexcept override { return drops.load(); }
  std::atomic<std::uint64_t> drops{0};
  Result<std::vector<EncodedPacket>> next(std::chrono::milliseconds timeout) override {
    ++calls;
    std::this_thread::sleep_for(timeout);
    return Result<std::vector<EncodedPacket>>::success({});
  }
  Result<std::vector<EncodedPacket>> flush() override {
    ++flushes;
    return Result<std::vector<EncodedPacket>>::success({});
  }
  std::atomic<std::size_t> calls{0};
  std::atomic<std::size_t> flushes{0};
};

TEST_CASE("audio mixing metrics sum sources, survive idle and reset for a new session") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto first = std::make_shared<IdleAudioProbe>();
  auto second = std::make_shared<IdleAudioProbe>();
  RecorderEngine engine({.clock = clock, .video = std::make_shared<FakeVideoEncoder>(),
      .audio = {first, second}, .hotkey = std::make_shared<FakeHotkeySource>(),
      .muxer = std::make_shared<BlockingMuxer>()});
  REQUIRE(engine.start(config()).is_success());
  first->drops = 24'000;
  second->drops = 12'000;
  REQUIRE(engine.metrics().audio_mixing_dropped_frames == 36'000);
  REQUIRE(engine.set_replay_enabled(false).is_success());
  REQUIRE(engine.metrics().audio_mixing_dropped_frames == 36'000);
  REQUIRE(engine.stop().is_success());
  REQUIRE(engine.metrics().audio_mixing_dropped_frames == 36'000);
  REQUIRE(engine.start(config()).is_success());
  REQUIRE(engine.metrics().audio_mixing_dropped_frames == 0);
  first->drops += 480;
  REQUIRE(engine.metrics().audio_mixing_dropped_frames == 480);
  REQUIRE(engine.stop().is_success());
}

class LifecycleVideoProbe final : public IEngineVideoPipeline {
 public:
  Result<void> resume() override {
    worker = std::this_thread::get_id();
    if (++resumes == 2 && fail_resume) return Result<void>::failure({"capture.injected_resume", "Injected resume failure", {}});
    return delegate.resume();
  }
  Result<std::vector<EncodedPacket>> tick(QpcTicks pts, bool keyframe) override {
    last_pts = pts;
    return delegate.tick(pts, keyframe);
  }
  Result<std::vector<EncodedPacket>> flush() override {
    auto result = delegate.flush();
    if (tail_packet) result.value().push_back({StreamKind::Video, 1, last_pts + 1,
        last_pts + 1, 1, false, std::make_shared<std::vector<std::byte>>(1)});
    return result;
  }
  void suspend() noexcept override {
    if (worker != std::this_thread::get_id()) wrong_thread = true;
    ++suspends;
  }
  FakeVideoEncoder delegate;
  std::atomic<int> resumes{0}, suspends{0};
  std::atomic_bool wrong_thread{false};
  bool fail_resume{false}, tail_packet{false};
 private:
  std::thread::id worker;
  QpcTicks last_pts{0};
};

TEST_CASE("failed resume releases pipeline on its worker and reports the failure", "[capture-idle]") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<LifecycleVideoProbe>();
  video->fail_resume = true;
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(), .muxer = std::make_shared<BlockingMuxer>()});
  REQUIRE(engine.start(config()).is_success());
  REQUIRE(video->delegate.wait_for_frames(1, 5s));
  REQUIRE(engine.set_replay_enabled(false).is_success());
  REQUIRE(video->suspends == 1);
  REQUIRE(engine.set_replay_enabled(true).is_success());
  for (int wait = 0; wait < 500 && video->suspends < 2; ++wait) std::this_thread::sleep_for(1ms);
  const auto stopped = engine.stop();
  REQUIRE_FALSE(stopped.is_success());
  REQUIRE(stopped.error().code == "capture.injected_resume");
  REQUIRE(video->suspends == 2);
  REQUIRE_FALSE(video->wrong_thread);
}

TEST_CASE("recording from idle drains video tail before writer finalization", "[capture-idle]") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<LifecycleVideoProbe>();
  video->tail_packet = true;
  auto recording = std::make_shared<FakeContinuousMuxer>();
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(), .muxer = std::make_shared<BlockingMuxer>(),
      .continuous_muxer = recording});
  REQUIRE(engine.start(config()).is_success());
  REQUIRE(video->delegate.wait_for_frames(1, 5s));
  REQUIRE(engine.set_replay_enabled(false).is_success());
  REQUIRE(engine.toggle_continuous_recording().is_success());
  REQUIRE(video->delegate.wait_for_frames(2, 5s));
  REQUIRE(engine.toggle_continuous_recording().is_success());
  REQUIRE(recording->finished);
  REQUIRE(recording->packets == 2); // First IDR plus the flush-only tail.
  REQUIRE(video->suspends == 2);
  REQUIRE_FALSE(video->wrong_thread);
  REQUIRE(engine.stop().is_success());
}

TEST_CASE("failed writer with replay off enters idle and replay can resume", "[capture-idle]") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<LifecycleVideoProbe>();
  auto recording = std::make_shared<FakeContinuousMuxer>();
  recording->fail = true;
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(), .muxer = std::make_shared<BlockingMuxer>(),
      .continuous_muxer = recording});
  REQUIRE(engine.start(config()).is_success());
  REQUIRE(video->delegate.wait_for_frames(1, 5s));
  REQUIRE(engine.set_replay_enabled(false).is_success());
  REQUIRE(engine.toggle_continuous_recording().is_success());
  REQUIRE(recording->wait_entered());
  for (int wait = 0; wait < 1000 && video->suspends < 2; ++wait) std::this_thread::sleep_for(1ms);
  REQUIRE(video->suspends == 2);
  clock->elapse(60'000);
  const auto count = video->delegate.timestamps().size();
  REQUIRE(engine.set_replay_enabled(true).is_success());
  REQUIRE(video->delegate.wait_for_frames(count + 1, 5s));
  REQUIRE(video->delegate.forced_keyframes()[count]);
  REQUIRE_FALSE(video->wrong_thread);
  REQUIRE_FALSE(engine.stop().is_success()); // The writer error remains visible (E10).
}

TEST_CASE("idle preserves queued saves and resumed replay has a fresh epoch and keyframe", "[capture-idle]") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto muxer = std::make_shared<BlockingMuxer>();
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(), .muxer = muxer});
  REQUIRE(engine.start(config()).is_success());
  clock->advance_through(60'000);
  REQUIRE(video->wait_for_frames(61, 5s));
  auto old_save = engine.save_clip(60'000);
  REQUIRE(muxer->wait_until_entered());
  const auto old_epoch = muxer->snapshot().packets.front().epoch;
  REQUIRE(engine.set_replay_enabled(false).is_success());
  const auto count = video->timestamps().size();
  clock->elapse(600'000 - clock->now());
  REQUIRE(engine.set_replay_enabled(true).is_success());
  clock->advance_through(660'000);
  REQUIRE(video->wait_for_frames(count + 61, 5s));
  muxer->release();
  REQUIRE(old_save.get().is_success());
  REQUIRE(video->forced_keyframes()[count]);
  REQUIRE(video->timestamps()[count] >= 600'000);
  auto new_save = engine.save_clip(660'000);
  REQUIRE(new_save.get().is_success());
  const auto resumed = muxer->snapshot();
  REQUIRE_FALSE(resumed.packets.empty());
  REQUIRE(std::all_of(resumed.packets.begin(), resumed.packets.end(),
      [&](const EncodedPacket& packet) { return packet.epoch > old_epoch && packet.pts >= 600'000; }));
  REQUIRE(engine.stop().is_success());
  REQUIRE(video->flushed_on_tick_thread());
}

TEST_CASE("continuous recording repeatedly wakes idle capture and returns to idle", "[capture-idle]") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto audio = std::make_shared<IdleAudioProbe>();
  auto recording = std::make_shared<FakeContinuousMuxer>();
  RecorderEngine engine({.clock = clock, .video = video, .audio = {audio},
      .hotkey = std::make_shared<FakeHotkeySource>(), .muxer = std::make_shared<BlockingMuxer>(),
      .continuous_muxer = recording});
  REQUIRE(engine.start(config()).is_success());
  REQUIRE(video->wait_for_frames(1, 5s));
  REQUIRE(engine.set_replay_enabled(false).is_success());
  for (int cycle = 0; cycle < 5; ++cycle) {
    const auto count = video->timestamps().size();
    clock->elapse((cycle + 1) * 120'000 - clock->now());
    REQUIRE(engine.toggle_continuous_recording().is_success());
    REQUIRE(video->wait_for_frames(count + 1, 5s));
    REQUIRE(video->forced_keyframes()[count]);
    REQUIRE(engine.toggle_continuous_recording().is_success());
    const auto ticks = engine.metrics().video_ticks;
    clock->advance_through((cycle + 1) * 120'000 + 60'000);
    std::this_thread::sleep_for(15ms);
    REQUIRE(engine.metrics().video_ticks == ticks);
    REQUIRE(video->flushed_on_tick_thread());
  }
  REQUIRE(audio->flushes == 6);
  REQUIRE(engine.stop().is_success());
}

class LateWakeClock final : public IEngineClock {
 public:
  QpcTicks now() const noexcept override { return clock_.now(); }
  QpcTicks frequency() const noexcept override { return clock_.frequency(); }
  bool wait_until(QpcTicks deadline, std::stop_token stop) override {
    {
      std::scoped_lock lock(mutex_);
      ++deadlines_;
    }
    condition_.notify_all();
    if (!clock_.wait_until(deadline, stop)) return false;
    clock_.elapse(2'500); // A loaded system wakes the capture worker after its deadline.
    return true;
  }
  bool wait_for_second_deadline() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, 5s, [&] { return deadlines_ >= 2; });
  }
 private:
  VirtualEngineClock clock_{60'000};
  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t deadlines_{0};
};

class PriorityProbeVideoPipeline final : public IEngineVideoPipeline {
 public:
  Result<std::vector<EncodedPacket>> tick(QpcTicks, bool) override {
    {
      std::scoped_lock lock(mutex_);
      priority_ = GetThreadPriority(GetCurrentThread());
    }
    condition_.notify_all();
    return Result<std::vector<EncodedPacket>>::success({});
  }
  Result<std::vector<EncodedPacket>> flush() override {
    return Result<std::vector<EncodedPacket>>::success({});
  }
  std::optional<int> wait_for_priority() {
    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, 5s, [&] { return priority_.has_value(); });
    return priority_;
  }
 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<int> priority_;
};

EngineConfig config() {
  EngineConfig value{};
  value.fps = 60;
  value.gop_seconds = 2;
  value.replay_capacity = 2s;
  value.clip_duration = 1s;
  value.output_directory = L"artifacts/test";
  value.system_audio_id = L"render-endpoint";
  value.microphone_id = L"capture-endpoint";
  return value;
}

}  // namespace

TEST_CASE("virtual 30 minute schedule has exact frame count and no accumulated drift") {
  constexpr QpcTicks frequency = 60'000;
  constexpr std::size_t frame_count = 30U * 60U * 60U;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto muxer = std::make_shared<BlockingMuxer>();
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
                         .muxer = muxer});

  REQUIRE(engine.start(config()).is_success());
  clock->advance_through(static_cast<QpcTicks>(frame_count - 1) * 1'000);
  REQUIRE(video->wait_for_frames(frame_count, 10s));

  const auto timestamps = video->timestamps();
  const auto keyframes = video->forced_keyframes();
  REQUIRE(timestamps.front() == 0);
  REQUIRE(timestamps.back() == static_cast<QpcTicks>(frame_count - 1) * 1'000);
  REQUIRE(keyframes[0]);
  REQUIRE(keyframes[119] == false);
  REQUIRE(keyframes[120]);
  REQUIRE(engine.stop().is_success());
  REQUIRE(engine.metrics().video_ticks == frame_count);
  REQUIRE(video->flushed());
  REQUIRE(video->flushed_on_tick_thread());
}

TEST_CASE("production scheduler clock uses a cancellable high resolution waitable timer") {
  auto created = WaitableTimerEngineClock::create();
  REQUIRE(created.is_success());
  auto clock = created.value();
  const auto started_at = clock->now();
  std::stop_source running;
  REQUIRE(clock->wait_until(started_at + clock->frequency() / 1'000,
                            running.get_token()));
  REQUIRE(clock->now() >= started_at);

  std::stop_source cancelled;
  cancelled.request_stop();
  REQUIRE_FALSE(clock->wait_until(clock->now() + clock->frequency(),
                                  cancelled.get_token()));
}

TEST_CASE("re-enabling replay requests a keyframe without continuous recording") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(),
      .muxer = std::make_shared<BlockingMuxer>()});

  REQUIRE(engine.start(config()).is_success());
  clock->advance_through(1'000);
  REQUIRE(video->wait_for_frames(2, 5s));
  REQUIRE(engine.set_replay_enabled(false).is_success());
  REQUIRE(engine.set_replay_enabled(true).is_success());
  clock->advance_through(2'000);
  REQUIRE(video->wait_for_frames(3, 5s));
  REQUIRE(engine.stop().is_success());

  const auto keyframes = video->forced_keyframes();
  REQUIRE(keyframes[0]);
  REQUIRE_FALSE(keyframes[1]);
  REQUIRE(keyframes[2]);
}

TEST_CASE("both consumers off joins capture while retaining control and save workers", "[capture-idle]") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto audio = std::make_shared<IdleAudioProbe>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  RecorderEngine engine({.clock = clock, .video = video, .audio = {audio},
      .hotkey = hotkey, .muxer = std::make_shared<BlockingMuxer>()});
  REQUIRE(engine.start(config()).is_success());
  clock->advance_through(1'000);
  REQUIRE(video->wait_for_frames(2, 5s));
  REQUIRE(engine.set_replay_enabled(false).is_success());
  const auto before = engine.metrics();
  const auto calls = audio->calls.load();
  const auto flushed = audio->flushes.load();
  const bool video_flushed = video->flushed();
  clock->advance_through(60'000);
  std::this_thread::sleep_for(30ms);
  const auto after = engine.metrics();
  const auto idle_calls = audio->calls.load();
  REQUIRE(engine.stop().is_success());
  REQUIRE(after.video_ticks == before.video_ticks);
  REQUIRE(idle_calls == calls);
  REQUIRE(flushed == 1);
  REQUIRE(video_flushed);
}

TEST_CASE("Replay memory pressure requests IDR without stopping scheduled capture", "[memory-budget]") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(),
      .muxer = std::make_shared<BlockingMuxer>()});
  auto settings = config();
  settings.maximum_buffer_bytes = 1024;
  REQUIRE(engine.start(settings).is_success());
  clock->advance_through(7'000);
  REQUIRE(video->wait_for_frames(8, 5s));
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (engine.metrics().video_ticks < 8 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto metrics = engine.metrics();
  REQUIRE(engine.stop().is_success());
  REQUIRE(metrics.video_ticks == 8);
  REQUIRE(metrics.budget_bytes == 1024);
  REQUIRE(metrics.buffered_bytes <= 1024);
  REQUIRE(metrics.replay_memory_limited);
  REQUIRE(metrics.replay_memory_drops > 0);
  REQUIRE(metrics.pipeline_errors == 0);
  const auto keyframes = video->forced_keyframes();
  REQUIRE(keyframes[0]);
  REQUIRE(std::count(keyframes.begin() + 1, keyframes.end(), true) > 0);
}

TEST_CASE("Replay toggle retains one shared budget across sixteen queued saves", "[memory-budget]") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto muxer = std::make_shared<BlockingMuxer>();
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(), .muxer = muxer});
  auto settings = config();
  settings.maximum_buffer_bytes = 8192;
  REQUIRE(engine.start(settings).is_success());
  clock->advance_through(0);
  REQUIRE(video->wait_for_frames(1, 5s));
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (engine.metrics().video_ticks < 1 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  std::vector<std::future<Result<std::filesystem::path>>> saves;
  for (int i = 0; i < 16; ++i) saves.push_back(engine.save_clip(0));
  const bool entered = muxer->wait_until_entered();
  auto seventeenth = engine.save_clip(0).get();
  const auto disabled = engine.set_replay_enabled(false);
  const auto held = engine.metrics();
  const auto enabled = engine.set_replay_enabled(true);
  const auto resumed = engine.metrics();
  muxer->release();
  const auto stopped = engine.stop();
  REQUIRE(entered);
  REQUIRE(disabled.is_success());
  REQUIRE(enabled.is_success());
  REQUIRE_FALSE(seventeenth.is_success());
  REQUIRE(seventeenth.error().code == "engine.save_queue_full");
  REQUIRE(held.replay_bytes == 0);
  REQUIRE(held.buffered_bytes > 0);
  REQUIRE(held.buffered_bytes <= 8192);
  REQUIRE(resumed.buffered_bytes == held.buffered_bytes);
  REQUIRE(resumed.budget_bytes == 8192);
  REQUIRE(stopped.is_success());
  for (auto& save : saves) REQUIRE(save.get().is_success());
}

TEST_CASE("video capture does not elevate its worker above ordinary game threads") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<PriorityProbeVideoPipeline>();
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(),
      .muxer = std::make_shared<BlockingMuxer>()});

  REQUIRE(engine.start(config()).is_success());
  const auto priority = video->wait_for_priority();
  REQUIRE(engine.stop().is_success());
  REQUIRE(priority.has_value());
  REQUIRE(*priority == THREAD_PRIORITY_NORMAL);
}

TEST_CASE("engine validates pinned audio selections and distinct hotkeys") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto muxer = std::make_shared<BlockingMuxer>();
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
                         .muxer = muxer});

  auto missing_system = config();
  missing_system.system_audio_id.clear();
  REQUIRE(engine.start(missing_system).error().code == "engine.system_audio_endpoint_required");

  auto missing_microphone = config();
  missing_microphone.microphone_id.clear();
  REQUIRE(engine.start(missing_microphone).error().code == "engine.microphone_endpoint_required");

  auto duplicate = config();
  duplicate.toggle_recording_hotkey = duplicate.save_replay_hotkey;
  REQUIRE(engine.start(duplicate).error().code == "engine.duplicate_hotkeys");

  auto disabled_audio = config();
  disabled_audio.system_audio_enabled = false;
  disabled_audio.microphone_enabled = false;
  disabled_audio.system_audio_id.clear();
  disabled_audio.microphone_id.clear();
  REQUIRE(engine.start(disabled_audio).is_success());
  REQUIRE(engine.stop().is_success());
}

TEST_CASE("startup buffers replay without creating files even when recording is enabled") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto clip_muxer = std::make_shared<BlockingMuxer>();
  auto continuous_muxer = std::make_shared<FakeContinuousMuxer>();
  RecorderEngine engine({.clock = clock, .video = video,
                         .hotkey = std::make_shared<FakeHotkeySource>(),
                         .muxer = clip_muxer, .continuous_muxer = continuous_muxer});
  auto enabled = config();
  enabled.continuous_recording_enabled = true;
  REQUIRE(engine.start(enabled).is_success());
  clock->advance_through(120'000);
  REQUIRE(video->wait_for_frames(121, 5s));
  REQUIRE_FALSE(engine.metrics().continuous_recording_active);
  REQUIRE(engine.metrics().completed_saves == 0);
  REQUIRE(clip_muxer->paths().empty());
  REQUIRE(continuous_muxer->destination.empty());
  REQUIRE(engine.stop().is_success());
}

TEST_CASE("explicit continuous recording retries an atomic destination collision") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto clip_muxer = std::make_shared<BlockingMuxer>();
  auto continuous_muxer = std::make_shared<FakeContinuousMuxer>();
  continuous_muxer->destination_collisions = 1;
  RecorderEngine engine({.clock = clock,
                         .video = video,
                         .hotkey = hotkey,
                         .muxer = clip_muxer,
                         .continuous_muxer = continuous_muxer});
  auto enabled = config();
  enabled.continuous_recording_enabled = true;

  REQUIRE(engine.start(enabled).is_success());
  REQUIRE_FALSE(engine.metrics().continuous_recording_active);
  REQUIRE(continuous_muxer->destination.empty());
  REQUIRE(engine.toggle_continuous_recording().is_success());
  REQUIRE(engine.metrics().continuous_recording_active);
  REQUIRE(continuous_muxer->destination.filename() == L"recording-000002.mp4");
  clock->advance_through(1'000);
  REQUIRE(video->wait_for_frames(2, 5s));
  REQUIRE(engine.stop().is_success());
}

TEST_CASE("continuous recording hotkey fans out packets while replay keeps advancing") {
  constexpr QpcTicks frequency = 60'000;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto clip_muxer = std::make_shared<BlockingMuxer>();
  auto continuous_muxer = std::make_shared<FakeContinuousMuxer>();
  RecorderEngine engine({.clock = clock,
                         .video = video,
                         .hotkey = hotkey,
                         .muxer = clip_muxer,
                         .continuous_muxer = continuous_muxer});

  REQUIRE(engine.start(config()).is_success());
  REQUIRE(hotkey->bindings() == std::vector<HotkeyBinding>{
                                    {HotkeyAction::SaveReplay, config().save_replay_hotkey},
                                    {HotkeyAction::ToggleRecording, config().toggle_recording_hotkey}});
  REQUIRE(video->wait_for_frames(1, 5s));

  hotkey->press(HotkeyAction::ToggleRecording, clock->now());
  // Hotkey delivery only enqueues work. Wait for the serialized control worker
  // to start the recording before advancing the virtual clock, so frame 1 is
  // the first post-request frame that must be an IDR frame.
  REQUIRE(continuous_muxer->wait_opened());
  clock->advance_through(3'000);
  REQUIRE(video->wait_for_frames(4, 5s));
  const auto while_recording = engine.metrics();
  REQUIRE(while_recording.continuous_recording_active);
  REQUIRE(while_recording.continuous_packets > 0);

  hotkey->press(HotkeyAction::ToggleRecording, clock->now());
  const auto replay_before = engine.metrics().video_packets;
  clock->advance_through(120'000);
  REQUIRE(video->wait_for_frames(121, 5s));
  REQUIRE(engine.stop().is_success());

  const auto completed = engine.metrics();
  REQUIRE(completed.video_packets > replay_before);
  REQUIRE_FALSE(completed.continuous_recording_active);
  REQUIRE(continuous_muxer->finished);
  REQUIRE(continuous_muxer->packets == completed.continuous_packets);
  REQUIRE(video->forced_keyframes()[1]);
  REQUIRE(video->forced_keyframes()[120]);
}

TEST_CASE("continuous recording retains its requested keyframe until a slow open completes") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto continuous_muxer = std::make_shared<FakeContinuousMuxer>();
  continuous_muxer->block_open = true;
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
                         .muxer = std::make_shared<BlockingMuxer>(),
                         .continuous_muxer = continuous_muxer});

  REQUIRE(engine.start(config()).is_success());
  REQUIRE(video->wait_for_frames(1, 5s));
  hotkey->press(HotkeyAction::ToggleRecording, clock->now());
  const bool opening = continuous_muxer->wait_open_entered();
  if (!opening) continuous_muxer->release_open();
  REQUIRE(opening);

  // Frames produced before the muxer can accept packets must not consume the
  // pending recording-start keyframe request.
  clock->advance_through(10'000);
  REQUIRE(video->wait_for_frames(11, 5s));
  const bool forced_while_opening = video->forced_keyframes()[1];

  continuous_muxer->release_open();
  REQUIRE(continuous_muxer->wait_opened());
  const auto ready_deadline = std::chrono::steady_clock::now() + 5s;
  while (!engine.metrics().continuous_recording_active &&
         std::chrono::steady_clock::now() < ready_deadline) {
    std::this_thread::yield();
  }
  REQUIRE(engine.metrics().continuous_recording_active);
  clock->advance_through(12'000);
  REQUIRE(video->wait_for_frames(13, 5s));
  const bool forced_after_open = video->forced_keyframes()[11];
  const bool accepted_after_open = engine.metrics().continuous_packets > 0;
  const auto toggled = engine.toggle_continuous_recording();
  const auto stopped = engine.stop();
  REQUIRE_FALSE(forced_while_opening);
  REQUIRE(forced_after_open);
  REQUIRE(accepted_after_open);
  REQUIRE(toggled.is_success());
  REQUIRE(stopped.is_success());
}

TEST_CASE("continuous recording stop never blocks replay behind a slow disk writer") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto muxer = std::make_shared<FakeContinuousMuxer>();
  muxer->blocked = true;
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
                         .muxer = std::make_shared<BlockingMuxer>(),
                         .continuous_muxer = muxer});
  auto enabled = config();
  enabled.continuous_recording_enabled = true;
  REQUIRE(engine.start(enabled).is_success());
  REQUIRE(engine.toggle_continuous_recording().is_success());
  clock->advance_through(1'000);
  const bool entered = muxer->wait_entered();
  if (!entered) muxer->release();
  REQUIRE(entered);
  auto stopping = std::async(std::launch::async, [&] {
    return engine.toggle_continuous_recording();
  });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (engine.metrics().continuous_recording_active &&
         std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  clock->advance_through(10'000);
  const bool replay_advanced = video->wait_for_frames(11, 1s);
  muxer->release();
  REQUIRE(stopping.get().is_success());
  REQUIRE(engine.stop().is_success());
  REQUIRE(replay_advanced);
}

TEST_CASE("continuous writer failure is visible while replay remains running") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto muxer = std::make_shared<FakeContinuousMuxer>();
  muxer->fail = true;
  RecorderEngine engine({.clock = clock, .video = video,
                         .hotkey = std::make_shared<FakeHotkeySource>(),
                         .muxer = std::make_shared<BlockingMuxer>(),
                         .continuous_muxer = muxer});
  auto enabled = config();
  enabled.continuous_recording_enabled = true;
  REQUIRE(engine.start(enabled).is_success());
  REQUIRE(engine.toggle_continuous_recording().is_success());
  clock->advance_through(1'000);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (engine.metrics().continuous_recording_active &&
         std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  const auto failed = engine.metrics();
  clock->advance_through(10'000);
  REQUIRE(video->wait_for_frames(11, 5s));
  REQUIRE(engine.stop().error().code == "mux.injected");
  REQUIRE_FALSE(failed.continuous_recording_active);
  REQUIRE(failed.continuous_failures == 1);
  REQUIRE(engine.metrics().continuous_failures == 1);
}

TEST_CASE("continuous recording restarts after disk failure without restarting the session") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<LifecycleVideoProbe>();
  auto muxer = std::make_shared<FakeContinuousMuxer>();
  muxer->fail = true;
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(), .muxer = std::make_shared<BlockingMuxer>(),
      .continuous_muxer = muxer});
  REQUIRE(engine.start(config()).is_success());
  REQUIRE(engine.toggle_continuous_recording().is_success());
  clock->advance_through(1'000);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (engine.metrics().continuous_failures == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  REQUIRE(engine.metrics().continuous_failures == 1);
  REQUIRE_FALSE(engine.metrics().continuous_recording_active);

  int expected_resumes = 1;
  SECTION("Replay keeps capturing during recovery") {}
  SECTION("recording resumes from capture idle") {
    REQUIRE(engine.set_replay_enabled(false).is_success());
    expected_resumes = 2;
  }
  std::filesystem::path failed_path;
  {
    std::scoped_lock lock(muxer->mutex);
    failed_path = muxer->destination;
    muxer->fail = false;
  }
  REQUIRE(engine.toggle_continuous_recording().is_success());
  REQUIRE(engine.metrics().continuous_recording_active);
  REQUIRE(engine.metrics().continuous_failures == 1); // Old error is not re-emitted.
  REQUIRE(muxer->wait_opened(2));
  clock->advance_through(10'000);
  REQUIRE(video->delegate.wait_for_frames(11, 5s));
  REQUIRE(engine.toggle_continuous_recording().is_success());
  REQUIRE(engine.stop().is_success());
  REQUIRE(video->resumes == expected_resumes);
  REQUIRE(engine.metrics().pipeline_errors == 0);
  REQUIRE(engine.metrics().continuous_failures == 1);
  REQUIRE(muxer->destination != failed_path);
  REQUIRE(muxer->finished);
  REQUIRE(muxer->packets > 0);
}

TEST_CASE("new open failure after writer failure is reported once and remains retryable") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto muxer = std::make_shared<FakeContinuousMuxer>();
  muxer->fail = true;
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(), .muxer = std::make_shared<BlockingMuxer>(),
      .continuous_muxer = muxer});
  REQUIRE(engine.start(config()).is_success());
  REQUIRE(engine.toggle_continuous_recording().is_success());
  clock->advance_through(1'000);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (engine.metrics().continuous_failures == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  REQUIRE(engine.metrics().continuous_failures == 1);
  {
    std::scoped_lock lock(muxer->mutex);
    muxer->fail = false;
    muxer->open_error = Error{"mux.reopen_failed", "Disk still unavailable", {}};
  }
  const auto failed = engine.toggle_continuous_recording();
  REQUIRE_FALSE(failed.is_success());
  REQUIRE(failed.error().code == "mux.reopen_failed");
  REQUIRE(engine.metrics().continuous_failures == 2);
  {
    std::scoped_lock lock(muxer->mutex);
    muxer->open_error.reset();
  }
  REQUIRE(engine.toggle_continuous_recording().is_success());
  clock->advance_through(10'000);
  REQUIRE(video->wait_for_frames(11, 5s));
  REQUIRE(engine.toggle_continuous_recording().is_success());
  REQUIRE(engine.stop().is_success());
  REQUIRE(engine.metrics().continuous_failures == 2);
}

TEST_CASE("writer failure discovered during stop is counted separately from a failed restart") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto muxer = std::make_shared<FakeContinuousMuxer>();
  muxer->blocked = true;
  muxer->fail = true;
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(), .muxer = std::make_shared<BlockingMuxer>(),
      .continuous_muxer = muxer});
  REQUIRE(engine.start(config()).is_success());
  REQUIRE(engine.toggle_continuous_recording().is_success());
  clock->advance_through(1'000);
  const bool entered = muxer->wait_entered();
  if (!entered) muxer->release();
  REQUIRE(entered);
  auto stopping = std::async(std::launch::async, [&] { return engine.toggle_continuous_recording(); });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (engine.metrics().continuous_recording_active && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool inactive = !engine.metrics().continuous_recording_active;
  muxer->release();
  const auto stopped = stopping.get();
  REQUIRE(inactive);
  REQUIRE_FALSE(stopped.is_success());
  REQUIRE(stopped.error().code == "mux.injected");
  REQUIRE(engine.metrics().continuous_failures == 1);
  {
    std::scoped_lock lock(muxer->mutex);
    muxer->fail = false;
    muxer->open_error = Error{"mux.reopen_failed", "Disk still unavailable", {}};
  }
  const auto retry = engine.toggle_continuous_recording();
  REQUIRE_FALSE(retry.is_success());
  REQUIRE(retry.error().code == "mux.reopen_failed");
  REQUIRE(engine.metrics().continuous_failures == 2);
  REQUIRE(engine.stop().is_success()); // Failed reopen already cleared the old writer error.
}

TEST_CASE("recording stop hotkey leaves the shared dispatcher free to snapshot replay") {
  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto clip_muxer = std::make_shared<BlockingMuxer>();
  auto recording_muxer = std::make_shared<FakeContinuousMuxer>();
  recording_muxer->blocked = true;
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
                         .muxer = clip_muxer, .continuous_muxer = recording_muxer});
  auto enabled = config();
  enabled.continuous_recording_enabled = true;
  REQUIRE(engine.start(enabled).is_success());
  REQUIRE(engine.toggle_continuous_recording().is_success());
  clock->advance_through(1'000);
  const bool entered = recording_muxer->wait_entered();
  if (!entered) recording_muxer->release();
  REQUIRE(entered);
  clock->advance_through(120'000);
  REQUIRE(video->wait_for_frames(121, 5s));
  // One thread invokes both callbacks in order, matching RawInputHotkey's
  // shared message dispatcher. A blocking toggle prevents the second delivery.
  auto dispatcher = std::async(std::launch::async, [&] {
    hotkey->press(HotkeyAction::ToggleRecording, 120'000);
    hotkey->press(HotkeyAction::SaveReplay, 120'000);
  });
  const bool save_delivered = clip_muxer->wait_until_entered();
  // Advance farther than Replay capacity while recording IO is still blocked.
  // The prompt SaveReplay snapshot must retain the original request's footage.
  clock->advance_through(360'000);
  const bool replay_advanced = video->wait_for_frames(361, 5s);
  const auto captured = clip_muxer->snapshot();
  recording_muxer->release();
  clip_muxer->release();
  dispatcher.get();
  REQUIRE(engine.stop().is_success());
  REQUIRE(save_delivered);
  REQUIRE(replay_advanced);
  REQUIRE(captured.end == 120'000);
  REQUIRE_FALSE(captured.packets.empty());
  REQUIRE(captured.packets.front().dts <= 60'000);
  REQUIRE(engine.metrics().completed_saves == 1);
}

TEST_CASE("scheduler counts and skips video deadlines missed by a slow pipeline") {
  constexpr QpcTicks frequency = 60'000;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<LaggingVideoPipeline>(clock);
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto muxer = std::make_shared<BlockingMuxer>();
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
                         .muxer = muxer});

  REQUIRE(engine.start(config()).is_success());
  clock->advance_through(1'000'000);
  REQUIRE(video->wait_for_frames(60));
  REQUIRE(engine.stop().is_success());
  REQUIRE(engine.metrics().missed_video_deadlines >= 4);
  const auto timestamps = video->timestamps();
  const auto keyframes = video->forced_keyframes();
  REQUIRE(keyframes.front());
  const auto secondKeyframe = std::find(keyframes.begin() + 1, keyframes.end(), true);
  REQUIRE(secondKeyframe != keyframes.end());
  REQUIRE(timestamps[static_cast<std::size_t>(secondKeyframe - keyframes.begin())] <= 127'000);
}

TEST_CASE("new recording session retries occupied numbered clips without overwrite") {
  TemporaryDirectory output;
  std::ofstream(output.path() / "clip-000001.mp4") << "existing";
  std::ofstream(output.path() / "clip-000003.mp4") << "keep-three";

  auto clock = std::make_shared<VirtualEngineClock>(60'000);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto muxer = std::make_shared<BlockingMuxer>();
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
                         .muxer = muxer});
  auto engine_config = config();
  engine_config.output_directory = output.path();

  REQUIRE(engine.start(engine_config).is_success());
  clock->advance_through(2 * clock->frequency());
  REQUIRE(video->wait_for_frames(121, 5s));
  auto saved = engine.save_clip(2 * clock->frequency());
  REQUIRE(muxer->wait_until_entered());
  muxer->release();
  const auto first = saved.get();
  REQUIRE(first.is_success());
  auto second = engine.save_clip(2 * clock->frequency()).get();
  REQUIRE(second.is_success());
  REQUIRE(engine.stop().is_success());
  REQUIRE(first.value().filename() == "clip-000002.mp4");
  REQUIRE(second.value().filename() == "clip-000004.mp4");
  REQUIRE(muxer->paths().size() == 3);
  std::string preserved_one;
  std::ifstream(output.path() / "clip-000001.mp4") >> preserved_one;
  REQUIRE(preserved_one == "existing");
  std::string preserved_three;
  std::ifstream(output.path() / "clip-000003.mp4") >> preserved_three;
  REQUIRE(preserved_three == "keep-three");
}

TEST_CASE("blocked save keeps capture running and owns an immutable snapshot") {
  constexpr QpcTicks frequency = 60'000;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto muxer = std::make_shared<BlockingMuxer>();
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
                         .muxer = muxer});

  REQUIRE(engine.start(config()).is_success());
  clock->advance_through(5 * frequency);
  REQUIRE(video->wait_for_frames(301, 5s));
  std::vector<std::future<Result<std::filesystem::path>>> saves;
  saves.push_back(engine.save_clip(5 * frequency));
  REQUIRE(muxer->wait_until_entered());
  for (int index = 1; index < 10; ++index) {
    saves.push_back(engine.save_clip(5 * frequency));
  }
  const auto frozen_packet_count = muxer->snapshot().packets.size();

  clock->advance_through(10 * frequency);
  REQUIRE(video->wait_for_frames(601, 5s));
  const auto packet_count_while_capture_continues = muxer->snapshot().packets.size();

  muxer->release();
  for (auto& saved : saves) {
    REQUIRE(saved.wait_for(5s) == std::future_status::ready);
    REQUIRE(saved.get().is_success());
  }
  REQUIRE(engine.stop().is_success());
  REQUIRE(packet_count_while_capture_continues == frozen_packet_count);
  REQUIRE(engine.metrics().video_ticks >= 601);
  REQUIRE(engine.metrics().completed_saves == 10);
}

TEST_CASE("bounded save queue counts the active mux operation") {
  constexpr QpcTicks frequency = 60'000;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto muxer = std::make_shared<BlockingMuxer>();
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
                         .muxer = muxer});
  auto bounded = config();
  bounded.maximum_pending_saves = 2;

  REQUIRE(engine.start(bounded).is_success());
  clock->advance_through(5 * frequency);
  REQUIRE(video->wait_for_frames(301, 5s));
  auto active = engine.save_clip(5 * frequency);
  REQUIRE(muxer->wait_until_entered());
  auto queued = engine.save_clip(5 * frequency);
  auto overflow = engine.save_clip(5 * frequency);
  const bool overflow_was_rejected_immediately =
      overflow.wait_for(0s) == std::future_status::ready;

  muxer->release();
  REQUIRE(active.get().is_success());
  REQUIRE(queued.get().is_success());
  const auto overflow_result = overflow.get();
  REQUIRE(engine.stop().is_success());
  REQUIRE(overflow_was_rejected_immediately);
  REQUIRE_FALSE(overflow_result.is_success());
  REQUIRE(overflow_result.error().code == "engine.save_queue_full");
  REQUIRE(engine.metrics().rejected_saves == 1);
}

TEST_CASE("audio packets append concurrently with the scheduled video pipeline") {
  constexpr QpcTicks frequency = 60'000;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto audio = std::make_shared<FiniteAudioPipeline>(100);
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto muxer = std::make_shared<BlockingMuxer>();
  RecorderEngine engine({.clock = clock, .video = video, .audio = {audio},
                         .hotkey = hotkey, .muxer = muxer});

  REQUIRE(engine.start(config()).is_success());
  clock->advance_through(2 * frequency);
  REQUIRE(video->wait_for_frames(121, 5s));
  REQUIRE(audio->wait_for_packets(100, 5s));
  REQUIRE(engine.stop().is_success());

  const auto metrics = engine.metrics();
  REQUIRE(metrics.video_packets == 121);
  REQUIRE(metrics.audio_packets == 100);
  REQUIRE(metrics.pipeline_errors == 0);
  REQUIRE(audio->flushed_on_next_thread());
}

TEST_CASE("hotkey dispatches a bounded save without pausing capture") {
  constexpr QpcTicks frequency = 60'000;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto muxer = std::make_shared<BlockingMuxer>();
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
                         .muxer = muxer});

  REQUIRE(engine.start(config()).is_success());
  clock->advance_through(5 * frequency);
  REQUIRE(video->wait_for_frames(301, 5s));
  hotkey->press(5 * frequency);
  REQUIRE(muxer->wait_until_entered());
  clock->advance_through(6 * frequency);
  REQUIRE(video->wait_for_frames(361, 5s));

  muxer->release();
  REQUIRE(engine.stop().is_success());
  const auto metrics = engine.metrics();
  REQUIRE(metrics.save_requests == 1);
  REQUIRE(metrics.completed_saves == 1);
  REQUIRE(metrics.last_hotkey_save_latency_ticks == 0);
  REQUIRE(metrics.video_ticks >= 361);
}

TEST_CASE("late capture wakeups cannot label a new image with an old video timestamp") {
  auto clock = std::make_shared<LateWakeClock>();
  auto video = std::make_shared<FakeVideoEncoder>();
  RecorderEngine engine({.clock = clock, .video = video,
      .hotkey = std::make_shared<FakeHotkeySource>(), .muxer = std::make_shared<BlockingMuxer>()});
  REQUIRE(engine.start(config()).is_success());
  REQUIRE(video->wait_for_frames(1, 5s));
  REQUIRE(clock->wait_for_second_deadline());
  REQUIRE(engine.stop().is_success());
  const auto timestamps = video->timestamps();
  REQUIRE(timestamps.front() == 2'500);
  REQUIRE(engine.metrics().missed_video_deadlines == 2);
}

TEST_CASE("queued replay saves keep the category captured at each request") {
  const bool direct_output = GENERATE(false, true);
  constexpr QpcTicks frequency = 60'000;
  TemporaryDirectory directory;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto muxer = std::make_shared<BlockingMuxer>();
  std::filesystem::path foreground = L"Counter-Strike 2";
  std::vector<std::filesystem::path> artwork_categories;
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
      .muxer = muxer, .capture_category = [&] { return foreground; },
      .cache_category_icon = [&](const std::filesystem::path& category) { artwork_categories.push_back(category); }});
  auto settings = config();
  settings.output_directory = directory.path();
  settings.save_without_game_folders = direct_output;
  REQUIRE(engine.start(settings).is_success());
  clock->advance_through(5 * frequency);
  REQUIRE(video->wait_for_frames(301, 5s));
  auto first = engine.save_clip(5 * frequency);
  REQUIRE(muxer->wait_until_entered());
  foreground = L"Desktop";
  auto second = engine.save_clip(5 * frequency);
  foreground = L"Different Game";
  muxer->release();
  const auto first_result = first.get();
  const auto second_result = second.get();
  REQUIRE(engine.stop().is_success());
  REQUIRE(first_result.is_success());
  REQUIRE(second_result.is_success());
  REQUIRE(first_result.value().parent_path() == (direct_output ? directory.path() : directory.path() / L"Counter-Strike 2"));
  REQUIRE(second_result.value().parent_path() == (direct_output ? directory.path() : directory.path() / L"Desktop"));
  REQUIRE(std::filesystem::is_directory(directory.path() / L"Desktop") == !direct_output);
  if (direct_output) {
    REQUIRE_FALSE(std::filesystem::exists(directory.path() / L"Counter-Strike 2"));
    REQUIRE(artwork_categories.empty());
  } else {
    REQUIRE(artwork_categories == std::vector<std::filesystem::path>{L"Counter-Strike 2", L"Desktop"});
  }
}

TEST_CASE("category callbacks cannot escape the clip root or break saving") {
  constexpr QpcTicks frequency = 60'000;
  TemporaryDirectory directory;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto muxer = std::make_shared<BlockingMuxer>();
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
      .muxer = muxer, .capture_category = [] { return std::filesystem::path(L"../escape"); },
      .cache_category_icon = [](const std::filesystem::path&) { throw std::runtime_error("Artwork unavailable"); }});
  auto settings = config();
  settings.output_directory = directory.path();
  REQUIRE(engine.start(settings).is_success());
  clock->advance_through(5 * frequency);
  REQUIRE(video->wait_for_frames(301, 5s));
  auto saved = engine.save_clip(5 * frequency);
  REQUIRE(muxer->wait_until_entered());
  muxer->release();
  const auto result = saved.get();
  REQUIRE(engine.stop().is_success());
  REQUIRE(result.is_success());
  REQUIRE(result.value().parent_path() == directory.path() / L"Desktop");
}

TEST_CASE("continuous recording keeps the hotkey category through a slow open and window switch") {
  const bool direct_output = GENERATE(false, true);
  constexpr QpcTicks frequency = 60'000;
  TemporaryDirectory directory;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  auto continuous_muxer = std::make_shared<FakeContinuousMuxer>();
  continuous_muxer->block_open = true;
  std::filesystem::path foreground = L"First Game";
  RecorderEngine engine({.clock = clock, .video = video, .hotkey = hotkey,
      .muxer = std::make_shared<BlockingMuxer>(), .continuous_muxer = continuous_muxer,
      .capture_category = [&] { return foreground; }});
  auto settings = config();
  settings.output_directory = directory.path();
  settings.save_without_game_folders = direct_output;
  REQUIRE(engine.start(settings).is_success());
  hotkey->press(HotkeyAction::ToggleRecording, clock->now());
  REQUIRE(continuous_muxer->wait_open_entered());
  foreground = L"Desktop";
  continuous_muxer->release_open();
  REQUIRE(continuous_muxer->wait_opened());
  clock->advance_through(frequency);
  REQUIRE(video->wait_for_frames(61, 5s));
  REQUIRE(engine.toggle_continuous_recording().is_success());
  REQUIRE(engine.stop().is_success());
  REQUIRE(continuous_muxer->destination.parent_path() == (direct_output ? directory.path() : directory.path() / L"First Game"));
  if (direct_output) REQUIRE_FALSE(std::filesystem::exists(directory.path() / L"First Game"));
}

TEST_CASE("naming preset is frozen at each save boundary and collisions keep originals") {
  const bool direct_output = GENERATE(false, true);
  constexpr QpcTicks frequency = 60'000;
  TemporaryDirectory directory;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto muxer = std::make_shared<BlockingMuxer>();
  const auto preset=directory.path()/L"recording-names.json";
  auto write=[&](const char* name){std::ofstream file(preset);file<<"{\"activeId\":\"user\",\"presets\":[{\"id\":\"user\",\"parts\":[{\"kind\":\"text\",\"value\":\""<<name<<"\"}]}]}";};
  write("First");
  const auto destination = direct_output ? directory.path() : directory.path()/L"First Game";
  std::filesystem::create_directories(destination);
  {std::ofstream existing(destination/L"First.mp4");existing<<"original";}
  RecorderEngine engine({.clock=clock,.video=video,.hotkey=std::make_shared<FakeHotkeySource>(),.muxer=muxer,.capture_category=[] {return std::filesystem::path(L"First Game");}});
  auto settings=config();settings.output_directory=directory.path();settings.naming_settings_file=preset;
  settings.save_without_game_folders=direct_output;
  REQUIRE(engine.start(settings).is_success());clock->advance_through(5*frequency);REQUIRE(video->wait_for_frames(301,5s));
  auto first=engine.save_clip(5*frequency);REQUIRE(muxer->wait_until_entered());write("Second");REQUIRE(engine.reload_recording_names().is_success());auto second=engine.save_clip(5*frequency);muxer->release();
  const auto a=first.get(),b=second.get();REQUIRE(engine.stop().is_success());REQUIRE(a.is_success());REQUIRE(b.is_success());
  REQUIRE(a.value().filename()==L"First (2).mp4");REQUIRE(b.value().filename()==L"Second.mp4");
  REQUIRE(a.value().parent_path()==destination);REQUIRE(b.value().parent_path()==destination);
  std::ifstream original(destination/L"First.mp4");std::string contents;original>>contents;REQUIRE(contents=="original");
}

TEST_CASE("sixteen hotkey saves freeze cached names while the writer is blocked") {
  constexpr QpcTicks frequency = 60'000;
  TemporaryDirectory directory;
  auto clock = std::make_shared<VirtualEngineClock>(frequency);
  auto video = std::make_shared<FakeVideoEncoder>();
  auto muxer = std::make_shared<BlockingMuxer>();
  auto hotkey = std::make_shared<FakeHotkeySource>();
  const auto preset = directory.path()/L"recording-names.json";
  auto write = [&](const char* prefix) {
    std::ofstream file(preset);
    file << "{\"activeId\":\"user\",\"presets\":[{\"id\":\"user\",\"parts\":[{\"kind\":\"text\",\"value\":\""
         << prefix << "\"},{\"kind\":\"token\",\"value\":\"counter\"}]}]}";
  };
  write("Cached ");
  RecorderEngine engine({.clock=clock,.video=video,.hotkey=hotkey,.muxer=muxer});
  auto settings = config();
  settings.output_directory = directory.path();
  settings.naming_settings_file = preset;
  settings.maximum_pending_saves = 16;
  REQUIRE(engine.start(settings).is_success());
  clock->advance_through(5*frequency);
  REQUIRE(video->wait_for_frames(301,5s));
  hotkey->press(5*frequency);
  REQUIRE(muxer->wait_until_entered());
  // Editing disk alone must not cause reads or change the cached request names.
  write("Updated ");
  std::vector<double> latency_us;
  for (int index=1;index<16;++index) {
    const auto started=std::chrono::steady_clock::now();
    hotkey->press(5*frequency);
    latency_us.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-started).count());
  }
  // Update after enqueue: already accepted names must remain frozen.
  REQUIRE(engine.reload_recording_names().is_success());
  const auto queued=engine.metrics();
  muxer->release();
  REQUIRE(engine.stop().is_success());
  REQUIRE(queued.save_requests==16);
  REQUIRE(queued.rejected_saves==0);
  REQUIRE(engine.metrics().completed_saves==16);
  const auto paths=muxer->paths();
  REQUIRE(paths.size()==16);
  for (std::size_t index=0;index<paths.size();++index) {
    REQUIRE(paths[index].filename()==L"Cached "+std::to_wstring(index+1)+L".mp4");
  }
  std::ifstream counter(directory.path()/L"recording-counter.txt");
  std::uint64_t persisted=0;counter>>persisted;
  REQUIRE(persisted==16);
  std::sort(latency_us.begin(),latency_us.end());
  std::cout << "Cached-name hotkey latency (15 calls, blocked writer): median_us="
            << latency_us[latency_us.size()/2] << "; max_us=" << latency_us.back() << '\n';
}
