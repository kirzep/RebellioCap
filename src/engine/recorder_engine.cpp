#include "engine/recorder_engine.h"
#include "engine/packet_budget_policy.h"
#include "engine/recording_name.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include <Windows.h>
#include <avrt.h>

#include "platform/windows/unique_handle.h"
#include "replay/replay_ring.h"
#include "engine/continuous_recording.h"

namespace rebelliocap {
namespace {

// Only audio needs multimedia scheduling to service its device buffers.
// Video conversion, encoding and replay bookkeeping must not outrank the game.
class AudioThreadScheduling {
 public:
  AudioThreadScheduling() {
    DWORD index = 0;
    task_ = AvSetMmThreadCharacteristicsW(L"Audio", &index);
    if (task_) AvSetMmThreadPriority(task_, AVRT_PRIORITY_HIGH);
  }
  ~AudioThreadScheduling() { if (task_) AvRevertMmThreadCharacteristics(task_); }
  AudioThreadScheduling(const AudioThreadScheduling&) = delete;
  AudioThreadScheduling& operator=(const AudioThreadScheduling&) = delete;
 private:
  HANDLE task_{};
};

Error engine_error(std::string code, std::string message) {
  return {.code = std::move(code), .message = std::move(message), .hresult = std::nullopt};
}

Result<void> validate_config(const EngineConfig& config, QpcTicks frequency) {
  if (frequency <= 0 || config.fps == 0 || config.gop_seconds == 0 ||
      config.replay_capacity <= std::chrono::seconds::zero() ||
      config.clip_duration <= std::chrono::seconds::zero() ||
      config.clip_duration > config.replay_capacity ||
      config.output_directory.empty() || config.maximum_pending_saves == 0) {
    return Result<void>::failure(engine_error(
        "engine.invalid_config", "Recorder configuration contains an invalid timing, path, or queue value."));
  }
  if (config.system_audio_enabled && config.system_audio_id.empty()) {
    return Result<void>::failure(engine_error(
        "engine.system_audio_endpoint_required",
        "System audio requires a pinned active render endpoint ID."));
  }
  if (config.microphone_enabled && config.microphone_id.empty()) {
    return Result<void>::failure(engine_error(
        "engine.microphone_endpoint_required",
        "Microphone audio requires a pinned active capture endpoint ID."));
  }
  const auto replay_hotkey = validate_hotkey_chord(config.save_replay_hotkey);
  if (!replay_hotkey.is_success()) return replay_hotkey;
  const auto recording_hotkey = validate_hotkey_chord(config.toggle_recording_hotkey);
  if (!recording_hotkey.is_success()) return recording_hotkey;
  if (config.save_replay_hotkey == config.toggle_recording_hotkey) {
    return Result<void>::failure(engine_error(
        "engine.duplicate_hotkeys", "Replay and recording hotkeys must be distinct."));
  }
  return Result<void>::success();
}

QpcTicks seconds_to_ticks(std::chrono::seconds value, QpcTicks frequency) {
  const auto seconds = value.count();
  if (seconds > 0 && frequency > (std::numeric_limits<QpcTicks>::max)() / seconds) {
    return (std::numeric_limits<QpcTicks>::max)();
  }
  return seconds * frequency;
}

QpcTicks frame_deadline(QpcTicks origin, std::uint64_t frame_index,
                        std::uint32_t fps, QpcTicks frequency) {
  const auto whole_seconds = frame_index / fps;
  const auto partial_frames = frame_index % fps;
  return origin + static_cast<QpcTicks>(whole_seconds) * frequency +
         static_cast<QpcTicks>(partial_frames) * frequency / fps;
}

}  // namespace

struct WaitableTimerEngineClock::ClockImpl {
  QpcClock clock;
  UniqueHandle timer;
  UniqueHandle stop_event;
};

Result<std::shared_ptr<WaitableTimerEngineClock>> WaitableTimerEngineClock::create() {
  auto implementation = std::make_unique<ClockImpl>();
  implementation->timer.reset(CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
  if (!implementation->timer) {
    const auto error = static_cast<long>(HRESULT_FROM_WIN32(GetLastError()));
    return Result<std::shared_ptr<WaitableTimerEngineClock>>::failure(
        {.code = "engine.timer_unavailable",
         .message = "A high-resolution waitable timer could not be created.",
         .hresult = error});
  }
  implementation->stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!implementation->stop_event) {
    const auto error = static_cast<long>(HRESULT_FROM_WIN32(GetLastError()));
    return Result<std::shared_ptr<WaitableTimerEngineClock>>::failure(
        {.code = "engine.timer_unavailable",
         .message = "The waitable timer cancellation event could not be created.",
         .hresult = error});
  }
  return Result<std::shared_ptr<WaitableTimerEngineClock>>::success(
      std::shared_ptr<WaitableTimerEngineClock>(
          new WaitableTimerEngineClock(std::move(implementation))));
}

WaitableTimerEngineClock::WaitableTimerEngineClock(
    std::unique_ptr<ClockImpl> implementation)
    : implementation_(std::move(implementation)) {}

WaitableTimerEngineClock::~WaitableTimerEngineClock() = default;

QpcTicks WaitableTimerEngineClock::now() const noexcept {
  return implementation_->clock.now();
}

QpcTicks WaitableTimerEngineClock::frequency() const noexcept {
  return implementation_->clock.frequency();
}

bool WaitableTimerEngineClock::wait_until(QpcTicks deadline, std::stop_token stop) {
  const auto current = now();
  if (deadline <= current) {
    return !stop.stop_requested();
  }
  if (ResetEvent(implementation_->stop_event.get()) == FALSE) {
    return false;
  }
  std::stop_callback wake(stop, [this] { SetEvent(implementation_->stop_event.get()); });
  const long double intervals = std::ceil(
      static_cast<long double>(deadline - current) * 10'000'000.0L /
      static_cast<long double>(frequency()));
  LARGE_INTEGER due{};
  due.QuadPart = -static_cast<LONGLONG>(intervals);
  if (SetWaitableTimerEx(implementation_->timer.get(), &due, 0, nullptr, nullptr,
                         nullptr, 0) == FALSE) {
    return false;
  }
  const HANDLE waits[]{implementation_->timer.get(), implementation_->stop_event.get()};
  return WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0;
}

struct RecorderEngine::Impl {
  struct SaveRequest {
    std::uint64_t sequence;
    std::wstring name;
    std::uint64_t collisions{0};
    std::filesystem::path category;
    ReplaySnapshot snapshot;
    std::promise<Result<std::filesystem::path>> completion;
    bool notify_hotkey_completion{false};
  };

  struct RecordingControlRequest {
    std::filesystem::path category;
    std::promise<Result<void>> completion;
  };

  explicit Impl(RecorderEngineDependencies value) : dependencies(std::move(value)) {}

  RecorderEngineDependencies dependencies;
  EngineConfig config{};
  std::unique_ptr<RecordingNames> names;
  std::unique_ptr<ReplayRing> replay;
  std::shared_ptr<PacketMemoryBudget> buffer_budget;
  mutable std::mutex replay_mutex;
  std::atomic_bool replay_enabled{true};
  std::unique_ptr<ContinuousRecording> continuous;
  std::atomic_bool running{false};
  std::jthread video_thread;
  std::vector<std::jthread> audio_threads;
  // Controls join/restart only. Workers never acquire this mutex.
  std::mutex capture_mutex;
  std::uint32_t capture_epoch{0};
  std::jthread save_thread;
  std::jthread recording_control_thread;
  std::mutex recording_control_mutex;
  std::condition_variable recording_control_condition;
  std::deque<std::shared_ptr<RecordingControlRequest>> recording_controls;
  bool accepting_recording_controls{false};
  bool recording_state_changed{false};
  static constexpr std::size_t kRecordingControlCapacity = 16;
  mutable std::mutex lifecycle_mutex;
  std::mutex save_mutex;
  std::condition_variable save_condition;
  std::deque<std::shared_ptr<SaveRequest>> save_queue;
  bool accepting_saves{false};
  std::uint64_t next_save_sequence{1};
  std::uint64_t next_recording_sequence{1};
  std::mutex continuous_mutex;
  std::atomic_bool recording_keyframe_requested{false};
  std::atomic_bool replay_keyframe_requested{false};

  std::atomic<std::uint64_t> video_ticks{0};
  std::atomic<std::uint64_t> missed_video_deadlines{0};
  std::atomic<std::uint64_t> video_packets{0};
  std::atomic<std::uint64_t> audio_packets{0};
  std::uint64_t audio_mixing_baseline{0};
  std::atomic<std::uint64_t> save_requests{0};
  std::atomic<std::uint64_t> completed_saves{0};
  std::atomic<std::uint64_t> failed_saves{0};
  std::atomic<std::uint64_t> rejected_saves{0};
  std::atomic<std::uint64_t> pipeline_errors{0};
  std::atomic<std::uint64_t> continuous_failures{0};
  std::atomic<QpcTicks> last_hotkey_save_latency_ticks{0};
  std::atomic<std::size_t> outstanding_saves{0};
  std::mutex save_completion_mutex;
  std::vector<EngineSaveCompletion> hotkey_save_completions;
  std::mutex failure_mutex;
  std::optional<Error> terminal_failure;
  void notify_state_changed() const noexcept { try { if (dependencies.state_changed) dependencies.state_changed(); } catch (...) {} }
  void notify_continuous_changed() {
    { std::scoped_lock lock(recording_control_mutex); recording_state_changed = true; }
    recording_control_condition.notify_one(); notify_state_changed();
  }

  void reset_metrics() noexcept {
    video_ticks = 0;
    missed_video_deadlines = 0;
    video_packets = 0;
    audio_packets = 0;
    audio_mixing_baseline = 0;
    for (const auto& audio : dependencies.audio) audio_mixing_baseline += audio->mixing_dropped_frames();
    save_requests = 0;
    completed_saves = 0;
    failed_saves = 0;
    rejected_saves = 0;
    pipeline_errors = 0;
    recording_keyframe_requested = false;
    replay_keyframe_requested = false;
    continuous_failures = 0;
    last_hotkey_save_latency_ticks = 0;
    outstanding_saves = 0;
    {
      std::scoped_lock completion_lock(save_completion_mutex);
      hotkey_save_completions.clear();
    }
    std::scoped_lock lock(failure_mutex);
    terminal_failure.reset();
  }

  void record_failure(Error error) {
    std::scoped_lock lock(failure_mutex);
    if (!terminal_failure.has_value()) {
      terminal_failure = std::move(error);
    }
    notify_state_changed();
  }

  bool capture_needed() const {
    return replay_enabled.load() || (continuous && continuous->active());
  }

  void stop_capture_locked() {
    video_thread.request_stop();
    for (auto& thread : audio_threads) thread.request_stop();
    if (video_thread.joinable()) video_thread.join();
    for (auto& thread : audio_threads) {
      if (thread.joinable()) thread.join();
    }
    audio_threads.clear();
  }

  Result<void> start_capture_locked() {
    if (capture_epoch == (std::numeric_limits<std::uint32_t>::max)()) {
      return Result<void>::failure(engine_error("engine.epoch_exhausted", "Capture epoch capacity reached."));
    }
    const auto epoch = ++capture_epoch;
    for (const auto& source : dependencies.audio) {
      audio_threads.emplace_back([this, source, epoch](std::stop_token stop) { run_audio(stop, source, epoch); });
    }
    video_thread = std::jthread([this, epoch](std::stop_token stop) { run_video(stop, epoch); });
    return Result<void>::success();
  }

  bool append_packets(std::vector<EncodedPacket> packets, bool video, std::uint32_t epoch) {
    for (auto& packet : packets) {
      packet.epoch = epoch;
      auto continuous_packet = packet;
      {
        std::scoped_lock replay_lock(replay_mutex);
        if (replay_enabled) {
          const bool was_limited = replay->memory_limited();
          const auto result = replay->append(std::move(packet));
          if (was_limited != replay->memory_limited()) notify_state_changed();
          if (!result.is_success() && result.error().code != "replay.memory_budget_exceeded") {
            ++pipeline_errors;
            record_failure(result.error());
            return false;
          }
          if (replay->memory_limited() && replay->bytes() == 0) {
            replay_keyframe_requested = true;
          }
        }
      }
      if (video) {
        ++video_packets;
      } else {
        ++audio_packets;
      }
      // The controller lives until capture workers have joined. Its queue lock
      // is independent of start/stop and disk IO, so Replay never waits for IO.
      if (continuous && continuous->active()) {
        static_cast<void>(continuous->accept(std::move(continuous_packet)));
      }
    }
    return true;
  }

  void run_video(std::stop_token stop, std::uint32_t epoch) {
    auto resumed = dependencies.video->resume();
    if (!resumed.is_success()) {
      ++pipeline_errors;
      record_failure(resumed.error());
      dependencies.video->suspend();
      return;
    }
    const auto origin = dependencies.clock->now();
    const auto frequency = dependencies.clock->frequency();
    const std::uint64_t keyframe_interval =
        static_cast<std::uint64_t>(config.fps) * config.gop_seconds;
    std::uint64_t next_keyframe_frame = 0;
    for (std::uint64_t frame = 0; !stop.stop_requested();) {
      const auto deadline = frame_deadline(origin, frame, config.fps, frequency);
      if (!dependencies.clock->wait_until(deadline, stop)) {
        break;
      }
      // Keep a toggle-start request pending while its muxer is opening. Frames
      // produced before the controller can accept them cannot be the recording
      // keyframe, because they are deliberately not fanned out to it.
      const bool recording_keyframe = continuous && continuous->active() &&
          recording_keyframe_requested.exchange(false);
      const bool replay_keyframe = replay_keyframe_requested.exchange(false);
      const bool force_keyframe = frame >= next_keyframe_frame ||
          recording_keyframe || replay_keyframe;
      // A deadline schedules work; it is not the time of the acquired image.
      // If Windows wakes us late, stamping that image with the old deadline
      // makes video content run ahead of QPC-stamped WASAPI audio.
      auto packets = dependencies.video->tick(dependencies.clock->now(), force_keyframe);
      if (!packets.is_success()) {
        ++pipeline_errors;
        record_failure(packets.error());
        break;
      }
      if (!append_packets(std::move(packets).value(), true, epoch)) {
        break;
      }
      if (frame >= next_keyframe_frame) {
        do {
          next_keyframe_frame += keyframe_interval;
        } while (next_keyframe_frame <= frame);
      }
      ++video_ticks;
      ++frame;
      const auto completed_at = dependencies.clock->now();
      while (!stop.stop_requested() &&
             frame_deadline(origin, frame, config.fps, frequency) < completed_at) {
        ++missed_video_deadlines;
        ++frame;
      }
    }
    auto tail = dependencies.video->flush();
    if (!tail.is_success()) {
      ++pipeline_errors;
      record_failure(tail.error());
    } else {
      static_cast<void>(append_packets(std::move(tail).value(), true, epoch));
    }
    dependencies.video->suspend();
  }

  void run_audio(std::stop_token stop, const std::shared_ptr<IEngineAudioPipeline>& source, std::uint32_t epoch) {
    const AudioThreadScheduling scheduling;
    auto resumed = source->resume();
    if (!resumed.is_success()) {
      ++pipeline_errors;
      record_failure(resumed.error());
      source->suspend();
      return;
    }
    auto unavailable = source->unavailable_audio_sources();
    while (!stop.stop_requested()) {
      auto packets = source->next(std::chrono::milliseconds(10));
      const auto current_unavailable = source->unavailable_audio_sources();
      if (current_unavailable != unavailable) {
        unavailable = current_unavailable;
        notify_state_changed();
      }
      if (!packets.is_success()) {
        ++pipeline_errors;
        record_failure(packets.error());
        break;
      }
      if (!append_packets(std::move(packets).value(), false, epoch)) {
        break;
      }
    }
    auto tail = source->flush();
    if (!tail.is_success()) {
      ++pipeline_errors;
      record_failure(tail.error());
    } else {
      static_cast<void>(append_packets(std::move(tail).value(), false, epoch));
    }
    source->suspend();
  }

  std::filesystem::path captured_category() const noexcept {
    if (!dependencies.capture_category) return {};
    try {
      auto category = dependencies.capture_category();
      const auto text = category.wstring();
      if (text.empty() || text == L"." || text == L".." || category.has_parent_path() ||
          text.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
          text.back() == L'.' || text.back() == L' ' ||
          std::any_of(text.begin(), text.end(), [](wchar_t c) { return c < 32; })) {
        return L"Desktop";
      }
      return category;
    } catch (...) { return L"Desktop"; }
  }

  Result<void> prepare_category(const std::filesystem::path& category) const {
    if (config.save_without_game_folders || category.empty()) return Result<void>::success();
    const auto directory = config.output_directory / category;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto attributes = GetFileAttributesW(directory.c_str());
    if (error || attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return Result<void>::failure(engine_error("engine.output_directory_failed",
          "Could not create a safe clip category directory."));
    }
    try {
      if (dependencies.cache_category_icon) dependencies.cache_category_icon(category);
    } catch (...) { /* Artwork failures cannot prevent recording. */ }
    return Result<void>::success();
  }

  std::wstring capture_name(const std::filesystem::path& category,bool recording,const SYSTEMTIME& time) {
    if(!names)return {};
    return names->capture(NameContext{time,category.empty()?L"Desktop":category.wstring(),1,recording,config.width,config.height,config.fps});
  }

  std::filesystem::path destination_directory(const std::filesystem::path& category) const {
    return config.save_without_game_folders ? config.output_directory : config.output_directory / category;
  }

  std::filesystem::path output_path(std::uint64_t sequence,
                                   const std::filesystem::path& category = {}, const std::wstring& stem = {},std::uint64_t collision=0) const {
    if(!stem.empty()){const auto suffix=collision?L" ("+std::to_wstring(collision+1)+L")":L"";return destination_directory(category)/(stem+suffix+(config.container==Container::Mp4?L".mp4":L".mkv"));}
    std::wostringstream name;
    name << L"clip-" << std::setw(6) << std::setfill(L'0') << sequence
         << (config.container == Container::Mp4 ? L".mp4" : L".mkv");
    return destination_directory(category) / name.str();
  }

  void complete_save(const std::shared_ptr<SaveRequest>& request,
                     Result<std::filesystem::path> result) {
    if (request->notify_hotkey_completion) {
      EngineSaveCompletion completion;
      if (result.is_success()) {
        completion.output_path = result.value();
      } else {
        completion.error = result.error();
      }
      std::scoped_lock lock(save_completion_mutex);
      hotkey_save_completions.push_back(std::move(completion));
    }
    request->completion.set_value(std::move(result));
    notify_state_changed();
  }

  std::future<Result<std::filesystem::path>> enqueue_save(
      QpcTicks requested_at, bool notify_hotkey_completion);

  std::filesystem::path recording_path(std::uint64_t sequence,
                                      const std::filesystem::path& category = {}, const std::wstring& stem = {},std::uint64_t collision=0) const {
    if(!stem.empty())return output_path(sequence,category,stem,collision);
    std::wostringstream name;
    name << L"recording-" << std::setw(6) << std::setfill(L'0') << sequence
         << (config.container == Container::Mp4 ? L".mp4" : L".mkv");
    return destination_directory(category) / name.str();
  }

  Result<void> start_continuous_locked(bool request_keyframe = false,
                                      const std::filesystem::path& category = {}) {
    if (!continuous) {
      return Result<void>::failure(engine_error(
          "engine.missing_continuous_muxer",
          "Continuous recording requires a streaming muxer."));
    }
    const auto prepared = prepare_category(category);
    if (!prepared.is_success()) return prepared;
    SYSTEMTIME name_time{};GetLocalTime(&name_time);
    const auto filename=capture_name(category,true,name_time);
    if(names)names->persist_counter();
    std::uint64_t collision=0;
    for (;;) {
      if (next_recording_sequence == (std::numeric_limits<std::uint64_t>::max)()) {
        return Result<void>::failure(engine_error(
            "engine.output_sequence_exhausted",
            "No available numbered recording destination remains."));
      }
      // A toggle takes effect at the request boundary: the next video frame
      // after this point must be independently decodable, even while opening
      // the destination takes time. Initial startup already emits frame zero
      // as a scheduled keyframe, so it does not need this extra request.
      if (request_keyframe) recording_keyframe_requested = true;
      auto started = continuous->start(recording_path(next_recording_sequence++, category,filename,collision++),
                                       config.container);
      if (request_keyframe && !started.is_success()) {
        recording_keyframe_requested = false;
      }
      if (started.is_success() || started.error().code != "mux.destination_exists") {
        return started;
      }
    }
  }

  Result<void> toggle_continuous(const std::filesystem::path& category) {
    std::scoped_lock lock(continuous_mutex);
    std::scoped_lock capture_lock(capture_mutex);
    if (!running.load()) {
      return Result<void>::failure(engine_error(
          "engine.not_running", "Recorder is not running."));
    }
    if (continuous && continuous->active()) {
      // Drain encoder tails into the still-active writer before finalization.
      if (!replay_enabled.load()) stop_capture_locked();
      return continuous->stop();
    }
    if (continuous) {
      // Join the failed writer before reopening its muxer. Its latched error
      // has already incremented the failure metric; start clears that error.
      // Returning it here would make every later start request fail forever.
      static_cast<void>(continuous->stop());
    }
    const bool from_idle = !replay_enabled.load();
    if (from_idle) stop_capture_locked();
    auto started = start_continuous_locked(true, category);
    if (!started.is_success()) {
      // An open/control failure is distinct from any previous writer failure.
      // Count at its origin, rather than comparing counters across a race with
      // the old writer. Active stop errors are already counted by the writer.
      ++continuous_failures;
      return started;
    }
    if (!from_idle) return started;
    auto resumed = start_capture_locked();
    if (!resumed.is_success()) {
      ++continuous_failures;
      static_cast<void>(continuous->stop());
    }
    return resumed;
  }

  std::future<Result<void>> enqueue_recording_control() {
    auto request = std::make_shared<RecordingControlRequest>();
    request->category = captured_category();
    auto future = request->completion.get_future();
    {
      std::scoped_lock lock(recording_control_mutex);
      if (!accepting_recording_controls || !running.load()) {
        request->completion.set_value(Result<void>::failure(
            engine_error("engine.not_running", "Recorder is not running.")));
        return future;
      }
      if (recording_controls.size() >= kRecordingControlCapacity) {
        ++continuous_failures;
        request->completion.set_value(Result<void>::failure(engine_error(
            "engine.recording_control_queue_full",
            "The bounded recording control queue is full.")));
        notify_state_changed();
        return future;
      }
      recording_controls.push_back(std::move(request));
    }
    recording_control_condition.notify_one();
    return future;
  }

  void run_recording_controls(std::stop_token stop) {
    for (;;) {
      std::shared_ptr<RecordingControlRequest> request;
      {
        std::unique_lock lock(recording_control_mutex);
        recording_control_condition.wait(lock, [&] {
          return stop.stop_requested() || recording_state_changed || !recording_controls.empty();
        });
        if (stop.stop_requested()) return;
        if (recording_controls.empty()) {
          recording_state_changed = false;
          lock.unlock();
          // Writer failure can remove the last consumer asynchronously. Make
          // the idle transition here, under the same lock as consumer changes;
          // workers must not exit on a stale consumer observation during resume.
          std::scoped_lock capture_lock(capture_mutex);
          if (running.load() && !capture_needed()) stop_capture_locked();
          continue;
        }
        request = std::move(recording_controls.front());
        recording_controls.pop_front();
      }
      auto result = toggle_continuous(request->category);
      request->completion.set_value(std::move(result));
      notify_state_changed();
    }
  }

  void stop_recording_controls() {
    {
      std::scoped_lock lock(recording_control_mutex);
      accepting_recording_controls = false;
      for (const auto& request : recording_controls) {
        request->completion.set_value(Result<void>::failure(
            engine_error("engine.not_running", "Recorder is stopping.")));
      }
      recording_controls.clear();
      recording_control_thread.request_stop();
    }
    recording_control_condition.notify_all();
    if (recording_control_thread.joinable()) recording_control_thread.join();
  }

  void run_saves(std::stop_token stop) {
    while (true) {
      std::shared_ptr<SaveRequest> request;
      {
        std::unique_lock lock(save_mutex);
        save_condition.wait(lock, [&] { return stop.stop_requested() || !save_queue.empty(); });
        if (save_queue.empty()) {
          if (stop.stop_requested()) {
            return;
          }
          continue;
        }
        request = std::move(save_queue.front());
        save_queue.pop_front();
      }

      if(names)names->persist_counter();
      const auto prepared = prepare_category(request->category);
      if (!prepared.is_success()) {
        ++failed_saves;
        complete_save(request, Result<std::filesystem::path>::failure(prepared.error()));
        --outstanding_saves;
        continue;
      }
      auto result = dependencies.muxer->write(
          request->snapshot, dependencies.stream_descriptors,
          output_path(request->sequence, request->category,request->name,request->collisions), config.container);
      while (!result.is_success() && result.error().code == "mux.destination_exists") {
        {
          std::scoped_lock lock(save_mutex);
          if (next_save_sequence == (std::numeric_limits<std::uint64_t>::max)()) {
            result = Result<std::filesystem::path>::failure(engine_error(
                "engine.output_sequence_exhausted",
                "No available numbered clip destination remains."));
            break;
          }
          request->sequence = next_save_sequence++;
          ++request->collisions;
        }
        result = dependencies.muxer->write(
            request->snapshot, dependencies.stream_descriptors,
            output_path(request->sequence, request->category,request->name,request->collisions), config.container);
      }
      if (result.is_success()) {
        ++completed_saves;
      } else {
        ++failed_saves;
      }
      complete_save(request, std::move(result));
      --outstanding_saves;
    }
  }

};

std::future<Result<std::filesystem::path>> RecorderEngine::Impl::enqueue_save(
    QpcTicks requested_at, bool notify_hotkey_completion) {
  SYSTEMTIME name_time{};GetLocalTime(&name_time);
  auto request = std::make_shared<SaveRequest>();
  request->category = captured_category();
  request->notify_hotkey_completion = notify_hotkey_completion;
  auto future = request->completion.get_future();
  {
    std::scoped_lock lock(save_mutex);
    if (!accepting_saves) {
      complete_save(request, Result<std::filesystem::path>::failure(
                                 engine_error("engine.not_running", "Recorder is not running.")));
      return future;
    }
    std::scoped_lock replay_lock(replay_mutex);
    if (!replay_enabled) {
      complete_save(request, Result<std::filesystem::path>::failure(engine_error("engine.replay_disabled", "Replay is disabled.")));
      return future;
    }
    if (outstanding_saves.load() >= config.maximum_pending_saves) {
      ++rejected_saves;
      complete_save(request, Result<std::filesystem::path>::failure(engine_error(
                                 "engine.save_queue_full",
                                 "The bounded save queue cannot accept another request.")));
      return future;
    }
    request->sequence = next_save_sequence++;
    ++outstanding_saves;
    ++save_requests;
    auto snapshot = replay->snapshot(
        requested_at, seconds_to_ticks(config.clip_duration, dependencies.clock->frequency()));
    if (!snapshot.is_success()) {
      --outstanding_saves;
      ++failed_saves;
      complete_save(request, Result<std::filesystem::path>::failure(
                                 std::move(snapshot).error()));
      return future;
    }
    request->name=capture_name(request->category,false,name_time);
    request->snapshot = std::move(snapshot).value();
    save_queue.push_back(request);
  }
  save_condition.notify_one();
  return future;
}

RecorderEngine::RecorderEngine(RecorderEngineDependencies dependencies)
    : implementation_(std::make_unique<Impl>(std::move(dependencies))) {}

RecorderEngine::~RecorderEngine() { static_cast<void>(stop()); }

Result<void> RecorderEngine::start(const EngineConfig& config) {
  auto& state = *implementation_;
  std::scoped_lock lifecycle_lock(state.lifecycle_mutex);
  if (state.running.load()) {
    return Result<void>::failure(engine_error("engine.already_running", "Recorder is already running."));
  }
  if (!state.dependencies.clock || !state.dependencies.video ||
      !state.dependencies.hotkey || !state.dependencies.muxer) {
    return Result<void>::failure(engine_error(
        "engine.missing_dependency", "Recorder dependencies are incomplete."));
  }
  if (config.continuous_recording_enabled && !state.dependencies.continuous_muxer) {
    return Result<void>::failure(engine_error(
        "engine.missing_continuous_muxer",
        "Continuous recording requires a streaming muxer."));
  }
  const auto valid = validate_config(config, state.dependencies.clock->frequency());
  if (!valid.is_success()) {
    return valid;
  }

  state.config = config;
  state.names=config.naming_settings_file.empty()?nullptr:std::make_unique<RecordingNames>(config.naming_settings_file);
  std::error_code output_error;
  std::uint64_t first_available_sequence = 1;
  while (std::filesystem::exists(state.output_path(first_available_sequence), output_error)) {
    if (first_available_sequence == (std::numeric_limits<std::uint64_t>::max)()) {
      return Result<void>::failure(engine_error(
          "engine.output_sequence_exhausted", "No available numbered clip destination remains."));
    }
    ++first_available_sequence;
  }
  if (output_error) {
    return Result<void>::failure(engine_error(
        "engine.output_directory_failed", "Could not inspect the clip output directory."));
  }
  state.next_recording_sequence = 1;
  while (std::filesystem::exists(state.recording_path(state.next_recording_sequence),
                                 output_error)) {
    if (state.next_recording_sequence == (std::numeric_limits<std::uint64_t>::max)()) {
      return Result<void>::failure(engine_error(
          "engine.output_sequence_exhausted",
          "No available numbered recording destination remains."));
    }
    ++state.next_recording_sequence;
  }
  if (output_error) {
    return Result<void>::failure(engine_error(
        "engine.output_directory_failed", "Could not inspect the recording output directory."));
  }
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  std::optional<PhysicalMemory> physical_memory;
  if (GlobalMemoryStatusEx(&memory)) {
    physical_memory=PhysicalMemory{memory.ullTotalPhys,memory.ullAvailPhys};
  }
  const auto policy=packet_budget_policy(config.replay_memory_limit_mb,physical_memory);
  if(!policy.is_success())return Result<void>::failure(policy.error());
  std::size_t memory_limit=policy.value();
  if (config.maximum_buffer_bytes != 0) {
    memory_limit = (std::min)(memory_limit, config.maximum_buffer_bytes);
  }
  state.buffer_budget = std::make_shared<PacketMemoryBudget>(memory_limit);
  state.replay = std::make_unique<ReplayRing>(
      seconds_to_ticks(config.replay_capacity, state.dependencies.clock->frequency()), state.buffer_budget);
  state.replay_enabled = true;
  state.continuous = state.dependencies.continuous_muxer
      ? std::make_unique<ContinuousRecording>(*state.dependencies.continuous_muxer,
                                              state.dependencies.stream_descriptors, state.buffer_budget, [&state] { state.notify_continuous_changed(); })
      : nullptr;
  state.reset_metrics();
  state.capture_epoch = 0;
  {
    std::scoped_lock save_lock(state.save_mutex);
    state.save_queue.clear();
    state.accepting_saves = true;
    state.next_save_sequence = first_available_sequence;
  }
  state.running = true;
  {
    std::scoped_lock control_lock(state.recording_control_mutex);
    state.accepting_recording_controls = true;
    state.recording_state_changed = false;
  }
  state.recording_control_thread = std::jthread(
      [&state](std::stop_token stop) { state.run_recording_controls(stop); });
  const std::array bindings{
      HotkeyBinding{.action = HotkeyAction::SaveReplay,
                    .chord = config.save_replay_hotkey},
      HotkeyBinding{.action = HotkeyAction::ToggleRecording,
                    .chord = config.toggle_recording_hotkey}};
  auto hotkey_result = state.dependencies.hotkey->start(
      bindings, [this](HotkeyAction action, QpcTicks timestamp) {
        if (action == HotkeyAction::ToggleRecording) {
          // RawInputHotkey delivers all bindings on one message thread. Only
          // enqueue here so a slow recording flush never delays Replay snapshots.
          static_cast<void>(implementation_->enqueue_recording_control());
          return;
        }
        static_cast<void>(implementation_->enqueue_save(timestamp, true));
        const auto completed_at = implementation_->dependencies.clock->now();
        implementation_->last_hotkey_save_latency_ticks =
            completed_at > timestamp ? completed_at - timestamp : 0;
      });
  if (!hotkey_result.is_success()) {
    state.stop_recording_controls();
    if (state.continuous) static_cast<void>(state.continuous->stop());
    {
      std::scoped_lock save_lock(state.save_mutex);
      state.accepting_saves = false;
    }
    state.running = false;
    state.replay.reset();
    state.continuous.reset();
    return hotkey_result;
  }

  state.save_thread = std::jthread([&state](std::stop_token stop) { state.run_saves(stop); });
  std::scoped_lock capture_lock(state.capture_mutex);
  return state.start_capture_locked();
}

std::future<Result<std::filesystem::path>> RecorderEngine::save_clip(QpcTicks requested_at) {
  return implementation_->enqueue_save(requested_at, false);
}

Result<void> RecorderEngine::toggle_continuous_recording() {
  return implementation_->enqueue_recording_control().get();
}

Result<void> RecorderEngine::reload_recording_names() {
  auto& state = *implementation_;
  std::scoped_lock lifecycle_lock(state.lifecycle_mutex);
  if (!state.running.load()) {
    return Result<void>::failure(engine_error("engine.not_running", "Recorder is not running."));
  }
  if(state.names)state.names->reload();
  return Result<void>::success();
}

Result<void> RecorderEngine::set_replay_enabled(bool enabled) {
  auto& state = *implementation_;
  std::scoped_lock lifecycle_lock(state.lifecycle_mutex);
  if (!state.running) return Result<void>::failure(engine_error("engine.not_running", "Session is stopped."));
  std::scoped_lock capture_lock(state.capture_mutex);
  if (state.replay_enabled.load() == enabled) return Result<void>::success();
  const bool no_recording = !state.continuous || !state.continuous->active();
  // No worker may deliver its old drain into the newly created ring.
  if (enabled && no_recording) state.stop_capture_locked();
  {
    std::scoped_lock replay_lock(state.replay_mutex);
    state.replay = std::make_unique<ReplayRing>(seconds_to_ticks(state.config.replay_capacity, state.dependencies.clock->frequency()), state.buffer_budget);
    state.replay_enabled = enabled;
    if (enabled) state.replay_keyframe_requested = true;
  }
  if (no_recording) {
    if (!enabled) state.stop_capture_locked();
    else return state.start_capture_locked();
  }
  return Result<void>::success();
}

bool RecorderEngine::replay_enabled() const {
  const auto& state = *implementation_;
  std::scoped_lock lock(state.replay_mutex);
  return state.running && state.replay_enabled;
}

Result<void> RecorderEngine::stop() {
  auto& state = *implementation_;
  std::unique_lock lifecycle_lock(state.lifecycle_mutex);
  if (!state.running.exchange(false)) {
    return Result<void>::success();
  }

  {
    std::scoped_lock save_lock(state.save_mutex);
    state.accepting_saves = false;
  }
  state.dependencies.hotkey->stop();
  {
    std::scoped_lock capture_lock(state.capture_mutex);
    state.stop_capture_locked();
  }

  state.stop_recording_controls();

  std::optional<Error> continuous_error;
  {
    std::scoped_lock continuous_lock(state.continuous_mutex);
    if (state.continuous) {
      auto stopped = state.continuous->stop();
      if (!stopped.is_success()) {
        continuous_error = stopped.error();
      }
    }
  }

  state.save_thread.request_stop();
  state.save_condition.notify_all();
  if (state.save_thread.joinable()) {
    state.save_thread.join();
  }
  state.replay.reset();
  {
    std::scoped_lock failure_lock(state.failure_mutex);
    if (state.terminal_failure.has_value()) {
      return Result<void>::failure(*state.terminal_failure);
    }
  }
  if (continuous_error.has_value()) return Result<void>::failure(*continuous_error);
  return Result<void>::success();
}

EngineMetrics RecorderEngine::metrics() const {
  const auto& state = *implementation_;
  std::scoped_lock lifecycle_lock(state.lifecycle_mutex);
  std::scoped_lock replay_lock(state.replay_mutex);
  std::uint64_t mixing_drops = 0;
  std::uint32_t unavailable = 0;
  for (const auto& audio : state.dependencies.audio) {
    mixing_drops += audio->mixing_dropped_frames();
    unavailable |= audio->unavailable_audio_sources();
  }
  return {.video_ticks = state.video_ticks.load(),
          .missed_video_deadlines = state.missed_video_deadlines.load(),
          .video_packets = state.video_packets.load(),
          .audio_packets = state.audio_packets.load(),
          .audio_mixing_dropped_frames = mixing_drops - state.audio_mixing_baseline,
          .audio_unavailable_sources = unavailable,
          .save_requests = state.save_requests.load(),
          .completed_saves = state.completed_saves.load(),
          .failed_saves = state.failed_saves.load(),
          .rejected_saves = state.rejected_saves.load(),
          .pipeline_errors = state.pipeline_errors.load(),
          .continuous_packets = state.continuous ? state.continuous->packets_accepted() : 0,
          .continuous_failures = state.continuous_failures.load() +
              (state.continuous ? state.continuous->failures() : 0),
          .continuous_recording_active = state.continuous && state.continuous->active(),
          .last_hotkey_save_latency_ticks = state.last_hotkey_save_latency_ticks.load(),
          .replay_bytes = state.replay ? state.replay->bytes() : 0,
          .budget_bytes = state.buffer_budget ? state.buffer_budget->limit() : 0,
          .buffered_bytes = state.buffer_budget ? state.buffer_budget->bytes() : 0,
          .replay_retained_seconds = state.replay
              ? static_cast<double>(state.replay->retained_duration()) / state.dependencies.clock->frequency() : 0,
          .replay_memory_limited = state.replay && state.replay->memory_limited(),
          .replay_memory_drops = state.replay ? state.replay->memory_drops() : 0,
          .continuous_recovery_path = state.continuous ? state.continuous->recovery_path() : ""};
}

std::vector<EngineSaveCompletion> RecorderEngine::take_hotkey_save_completions() {
  auto& state = *implementation_;
  std::scoped_lock lock(state.save_completion_mutex);
  return std::exchange(state.hotkey_save_completions, {});
}

}  // namespace rebelliocap
