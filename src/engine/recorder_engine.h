#pragma once

#include <chrono>
#include <filesystem>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

#include "core/qpc_clock.h"
#include "core/result.h"
#include "engine/engine_config.h"
#include "hotkey/hotkey_source.h"
#include "media/encoded_packet.h"
#include "media/stream_descriptor.h"
#include "mux/continuous_muxer.h"

namespace rebelliocap {

class IEngineClock {
 public:
  virtual ~IEngineClock() = default;
  [[nodiscard]] virtual QpcTicks now() const noexcept = 0;
  [[nodiscard]] virtual QpcTicks frequency() const noexcept = 0;
  virtual bool wait_until(QpcTicks deadline, std::stop_token stop) = 0;
};

class WaitableTimerEngineClock final : public IEngineClock {
 public:
  static Result<std::shared_ptr<WaitableTimerEngineClock>> create();
  ~WaitableTimerEngineClock() override;

  WaitableTimerEngineClock(const WaitableTimerEngineClock&) = delete;
  WaitableTimerEngineClock& operator=(const WaitableTimerEngineClock&) = delete;

  [[nodiscard]] QpcTicks now() const noexcept override;
  [[nodiscard]] QpcTicks frequency() const noexcept override;
  bool wait_until(QpcTicks deadline, std::stop_token stop) override;

 private:
  struct ClockImpl;
  explicit WaitableTimerEngineClock(std::unique_ptr<ClockImpl> implementation);
  std::unique_ptr<ClockImpl> implementation_;
};

// Task 12 supplies the hardware adapter that combines DXGI acquisition,
// latest-frame reuse, BGRA-to-NV12 conversion, and direct NVENC submission.
// Keeping that adapter behind one scheduled operation makes Task 11's timing
// and save-continuity behavior deterministic without weakening texture leases.
class IEngineVideoPipeline {
 public:
  virtual ~IEngineVideoPipeline() = default;
  // Called on the capture worker, including after an idle interval.
  virtual Result<void> resume() { return Result<void>::success(); }
  virtual void suspend() noexcept {}
  virtual Result<std::vector<EncodedPacket>> tick(QpcTicks pts,
                                                   bool force_keyframe) = 0;
  virtual Result<std::vector<EncodedPacket>> flush() = 0;
};

class IEngineAudioPipeline {
 public:
  virtual ~IEngineAudioPipeline() = default;
  // May be read concurrently with the audio worker; monotonically counts mixing
  // copies over this pipeline's lifetime, including across idle/flush cycles.
  [[nodiscard]] virtual std::uint64_t mixing_dropped_frames() const noexcept { return 0; }
  // Bit 0: system audio; bit 1: microphone. Atomic snapshot for UI/control.
  [[nodiscard]] virtual std::uint32_t unavailable_audio_sources() const noexcept { return 0; }
  virtual Result<void> resume() { return Result<void>::success(); }
  virtual void suspend() noexcept {}
  virtual Result<std::vector<EncodedPacket>> next(
      std::chrono::milliseconds timeout) = 0;
  virtual Result<std::vector<EncodedPacket>> flush() = 0;
};

struct RecorderEngineDependencies {
  std::shared_ptr<IEngineClock> clock;
  std::shared_ptr<IEngineVideoPipeline> video;
  std::vector<std::shared_ptr<IEngineAudioPipeline>> audio;
  std::shared_ptr<IHotkeySource> hotkey;
  std::shared_ptr<IClipMuxer> muxer;
  std::shared_ptr<IContinuousMuxer> continuous_muxer;
  std::vector<StreamDescriptor> stream_descriptors;
  // Snapshot the destination at the request boundary, before asynchronous muxing.
  std::function<std::filesystem::path()> capture_category;
  // Best-effort artwork cache, called only by save/recording workers after mkdir.
  std::function<void(const std::filesystem::path&)> cache_category_icon;
  // Signal readiness only; callbacks must not synchronously read engine state.
  std::function<void()> state_changed;
};

struct EngineSaveCompletion {
  std::optional<std::filesystem::path> output_path;
  std::optional<Error> error;
};

class RecorderEngine {
 public:
  explicit RecorderEngine(RecorderEngineDependencies dependencies);
  ~RecorderEngine();

  RecorderEngine(const RecorderEngine&) = delete;
  RecorderEngine& operator=(const RecorderEngine&) = delete;

  Result<void> start(const EngineConfig& config);
  std::future<Result<std::filesystem::path>> save_clip(QpcTicks requested_at);
  Result<void> toggle_continuous_recording();
  Result<void> set_replay_enabled(bool enabled);
  Result<void> reload_recording_names();
  [[nodiscard]] bool replay_enabled() const;
  Result<void> stop();
  [[nodiscard]] EngineMetrics metrics() const;
  std::vector<EngineSaveCompletion> take_hotkey_save_completions();

 private:
  struct Impl;
  std::unique_ptr<Impl> implementation_;
};

}  // namespace rebelliocap
