#include "config/recording_contract.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "engine/session_controller.h"
#include "engine/session_input.h"
#include "fakes/fake_capture_source.h"
#include "fakes/fake_video_encoder.h"
#include "fakes/fake_continuous_muxer.h"

using namespace rebelliocap;
using namespace rebelliocap::testing;
using namespace std::chrono_literals;
namespace {
class ClipMuxer final : public IClipMuxer {
 public:
  Result<std::filesystem::path> write(const ReplaySnapshot&,
      const std::vector<StreamDescriptor>&, const std::filesystem::path& path,
      Container) override {
    std::unique_lock lock(mutex);
    entered=true; condition.notify_all();
    condition.wait(lock,[&]{return !blocked;});
    if(failure) return Result<std::filesystem::path>::failure(*failure);
    return Result<std::filesystem::path>::success(path);
  }
  bool wait_entered() {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock,5s,[&]{return entered;});
  }
  void release() { std::scoped_lock lock(mutex); blocked=false; condition.notify_all(); }
  std::mutex mutex;
  std::condition_variable condition;
  std::optional<Error> failure;
  bool blocked{false};
  bool entered{false};
};
struct Fixture {
  std::shared_ptr<VirtualEngineClock> clock = std::make_shared<VirtualEngineClock>(60'000);
  std::shared_ptr<FakeVideoEncoder> video = std::make_shared<FakeVideoEncoder>();
  std::shared_ptr<FakeHotkeySource> hotkey = std::make_shared<FakeHotkeySource>();
  std::shared_ptr<ClipMuxer> muxer = std::make_shared<ClipMuxer>();
  RecorderEngineDependencies dependencies() {
    return {.clock=clock, .video=video, .hotkey=hotkey,
      .muxer=muxer,
      .continuous_muxer=std::make_shared<FakeContinuousMuxer>(),
      .stream_descriptors={{StreamKind::Video, "h264", "Video", {}, {1,60'000}, 1920,1080}}};
  }
  EngineConfig config() {
    EngineConfig c;
    c.system_audio_enabled=false; c.microphone_enabled=false;
    c.output_directory=std::filesystem::temp_directory_path();
    c.clip_duration=1s;
    return c;
  }
};
SessionRequest request(std::string id, SessionCommand command) { return {1,std::move(id),command}; }
}
TEST_CASE("session completion wakes host only after its future and metrics are ready") {
  Fixture f; f.muxer->blocked=true;
  auto wake=std::make_shared<SessionWake>(); auto dependencies=f.dependencies();
  dependencies.state_changed=[wake]{wake->notify();};
  SessionController controller(std::move(dependencies));
  REQUIRE(controller.start(f.config()).is_success());
  f.clock->advance_through(60'000);REQUIRE(f.video->wait_for_frames(61,5s));
  static_cast<void>(controller.take_events());
  REQUIRE(controller.handle(request("save-event",SessionCommand::SaveReplay)).is_success());
  REQUIRE(f.muxer->wait_entered());
  const auto before=wake->revision(); f.muxer->release();
  REQUIRE(wake->wait_until(before,std::chrono::steady_clock::now()+2s));
  controller.poll();
  auto events=controller.take_events();
  REQUIRE(std::any_of(events.begin(),events.end(),[](const auto& event){return event.type==SessionEventType::ClipSaved&&event.request_id=="save-event";}));
  REQUIRE(controller.snapshot().metrics.completed_saves==1);
  REQUIRE(controller.stop().is_success());
}
TEST_CASE("pipeline failure wakes host without waiting for the metrics heartbeat") {
  Fixture f; auto wake=std::make_shared<SessionWake>();auto dependencies=f.dependencies();
  class FailingVideo final : public IEngineVideoPipeline {
   public:
    std::atomic_bool failed{false};
    std::shared_ptr<FakeVideoEncoder> delegate;
    explicit FailingVideo(std::shared_ptr<FakeVideoEncoder> value):delegate(std::move(value)){}
    Result<void> resume() override {return delegate->resume();}
    Result<std::vector<EncodedPacket>> tick(QpcTicks pts,bool keyframe) override {
      if(failed.load())return Result<std::vector<EncodedPacket>>::failure({"test.video_failure","Injected pipeline failure",{}});
      return delegate->tick(pts,keyframe);
    }
    Result<std::vector<EncodedPacket>> flush() override {return delegate->flush();}
  };
  auto video=std::make_shared<FailingVideo>(f.video);dependencies.video=video;
  dependencies.state_changed=[wake]{wake->notify();};
  SessionController controller(std::move(dependencies));
  REQUIRE(controller.start(f.config()).is_success());
  f.clock->advance_through(1000);REQUIRE(f.video->wait_for_frames(2,5s));
  const auto before=wake->revision();
  video->failed.store(true);
  f.clock->advance_through(5000);
  REQUIRE(wake->wait_until(before,std::chrono::steady_clock::now()+2s));
  controller.poll();REQUIRE(controller.snapshot().lifecycle==EngineLifecycle::Failed);
}
TEST_CASE("toggle changes real continuous state and duplicate IDs cannot toggle it back") {
  Fixture f; SessionController c(f.dependencies());
  REQUIRE(c.start(f.config()).is_success());
  auto before=c.snapshot();
  REQUIRE(c.handle(request("r-2",SessionCommand::ToggleRecording)).is_success());
  f.clock->advance_through(60'000);
  REQUIRE(f.video->wait_for_frames(60,5s));
  auto after=c.snapshot();
  REQUIRE(after.continuous_recording_active);
  REQUIRE(after.revision>before.revision);
  REQUIRE_FALSE(c.handle(request("r-2",SessionCommand::ToggleRecording)).is_success());
  REQUIRE(c.snapshot().continuous_recording_active);
  REQUIRE(c.stop().is_success());
  REQUIRE(c.stop().is_success());
  REQUIRE(c.snapshot().lifecycle==EngineLifecycle::Stopped);
  REQUIRE_FALSE(c.snapshot().continuous_recording_active);
}

TEST_CASE("naming reload acknowledges control without enqueuing a replay save") {
  Fixture f; SessionController controller(f.dependencies());
  REQUIRE(controller.start(f.config()).is_success());
  static_cast<void>(controller.take_events());
  REQUIRE(controller.handle(request("names",SessionCommand::ReloadRecordingNames)).is_success());
  const auto events=controller.take_events();
  REQUIRE(events.size()==1);
  REQUIRE(events.front().type==SessionEventType::CommandResult);
  REQUIRE(events.front().request_id=="names");
  REQUIRE_FALSE(events.front().error.has_value());
  REQUIRE(controller.snapshot().metrics.save_requests==0);
  REQUIRE(controller.stop().is_success());
  REQUIRE_FALSE(controller.handle(request("names-stopped",SessionCommand::ReloadRecordingNames)).is_success());
}

TEST_CASE("memory pressure publishes a snapshot without stopping capture", "[memory-budget]") {
  Fixture f;
  SessionController controller(f.dependencies());
  auto config = f.config();
  config.maximum_buffer_bytes = 1024;
  REQUIRE(controller.start(config).is_success());
  static_cast<void>(controller.take_events());
  f.clock->advance_through(7'000);
  REQUIRE(f.video->wait_for_frames(8, 5s));
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!controller.snapshot().metrics.replay_memory_limited &&
      std::chrono::steady_clock::now() < deadline) {
    controller.poll();
    std::this_thread::yield();
  }
  const auto snapshot = controller.snapshot();
  const auto events = controller.take_events();
  REQUIRE(controller.stop().is_success());
  REQUIRE(snapshot.lifecycle == EngineLifecycle::Ready);
  REQUIRE(snapshot.metrics.replay_memory_limited);
  REQUIRE(std::any_of(events.begin(), events.end(), [](const auto& event) {
    return event.type == SessionEventType::Snapshot && event.snapshot &&
        event.snapshot->metrics.replay_memory_limited;
  }));
}
TEST_CASE("Replay and continuous recording operate independently") {
  Fixture f; SessionController c(f.dependencies());
  REQUIRE(c.start(f.config()).is_success());
  REQUIRE(c.handle(request("replay-off", SessionCommand::StopReplay)).is_success());
  REQUIRE_FALSE(c.snapshot().replay_active);
  REQUIRE(c.handle(request("recording-on", SessionCommand::ToggleRecording)).is_success());
  // Advance actual media before stopping; otherwise the result races the first
  // keyframe and legitimately reports recording.no_media.
  f.clock->advance_through(60'000);
  REQUIRE(f.video->wait_for_frames(61,5s));
  REQUIRE(c.snapshot().continuous_recording_active);
  REQUIRE_FALSE(c.snapshot().replay_active);
  REQUIRE(c.handle(request("replay-on", SessionCommand::StartReplay)).is_success());
  REQUIRE(c.snapshot().replay_active);
  REQUIRE(c.snapshot().continuous_recording_active);
  REQUIRE(c.handle(request("replay-off-again", SessionCommand::StopReplay)).is_success());
  REQUIRE_FALSE(c.snapshot().replay_active);
  REQUIRE(c.snapshot().continuous_recording_active);
  REQUIRE_FALSE(c.handle(request("save-disabled", SessionCommand::SaveReplay)).is_success());
  REQUIRE(c.handle(request("recording-off", SessionCommand::ToggleRecording)).is_success());
  REQUIRE_FALSE(c.snapshot().continuous_recording_active);
  REQUIRE(c.handle(request("replay-only", SessionCommand::StartReplay)).is_success());
  REQUIRE(c.snapshot().replay_active);
  REQUIRE_FALSE(c.snapshot().continuous_recording_active);
  REQUIRE(c.stop().is_success());
}
TEST_CASE("save completion is correlated and contains the completed output path") {
  Fixture f; SessionController c(f.dependencies());
  REQUIRE(c.start(f.config()).is_success());
  f.clock->advance_through(120'000);
  REQUIRE(f.video->wait_for_frames(120,5s));
  REQUIRE(c.handle(request("save-1",SessionCommand::SaveReplay)).is_success());
  REQUIRE(c.stop().is_success());
  c.poll();
  auto events=c.take_events();
  bool saved=false;
  for (const auto& event:events) if(event.type==SessionEventType::ClipSaved) {
    REQUIRE(event.request_id=="save-1");
    REQUIRE(event.output_path.has_value());
    saved=true;
  }
  REQUIRE(saved);
  std::uint64_t revision=0;
  for (const auto& event:events) if(event.snapshot) {
    REQUIRE(event.snapshot->revision>=revision); revision=event.snapshot->revision;
  }
}
TEST_CASE("native hotkey save completion publishes an uncorrelated clip event") {
  Fixture f; SessionController c(f.dependencies());
  REQUIRE(c.start(f.config()).is_success());
  c.take_events();
  f.clock->advance_through(120'000);
  REQUIRE(f.video->wait_for_frames(120,5s));
  f.hotkey->press(HotkeyAction::SaveReplay,f.clock->now());
  bool completed=false;
  for(int n=0;n<500 && !completed;++n) {
    std::this_thread::sleep_for(1ms); c.poll();
    for(const auto& event:c.take_events()) if(event.type==SessionEventType::ClipSaved) {
      REQUIRE_FALSE(event.request_id.has_value());
      REQUIRE(event.output_path.has_value());
      completed=true;
    }
  }
  REQUIRE(completed);
  REQUIRE(c.stop().is_success());
}
TEST_CASE("native hotkey save failure publishes an uncorrelated error event") {
  Fixture f;
  f.muxer->failure=Error{"mux.hotkey_failure","Injected hotkey save failure",{}};
  SessionController c(f.dependencies());
  REQUIRE(c.start(f.config()).is_success());
  c.take_events();
  f.clock->advance_through(120'000);
  REQUIRE(f.video->wait_for_frames(120,5s));
  f.hotkey->press(HotkeyAction::SaveReplay,f.clock->now());
  bool completed=false;
  for(int n=0;n<500 && !completed;++n) {
    std::this_thread::sleep_for(1ms); c.poll();
    for(const auto& event:c.take_events()) if(event.type==SessionEventType::Error) {
      REQUIRE_FALSE(event.request_id.has_value());
      REQUIRE(event.error->code=="mux.hotkey_failure");
      REQUIRE_FALSE(event.output_path.has_value());
      completed=true;
    }
  }
  REQUIRE(completed);
  REQUIRE(c.stop().is_success());
}
TEST_CASE("bounded test stops capture while delayed mux completion is pending") {
  Fixture f; f.muxer->blocked=true;
  RecorderEngine engine(f.dependencies());
  REQUIRE(engine.start(f.config()).is_success());
  f.clock->advance_through(120'000);
  REQUIRE(f.video->wait_for_frames(120,5s));
  auto completion=std::async(std::launch::async,[&] {
    return finish_bounded_recording(engine,f.clock->now());
  });
  const bool entered=f.muxer->wait_entered();
  if(!entered) f.muxer->release();
  REQUIRE(entered);
  for(int n=0;n<500 && !f.video->flushed();++n) std::this_thread::sleep_for(1ms);
  REQUIRE(f.video->flushed());
  const auto stopped_ticks=f.video->timestamps().size();
  f.clock->advance_through(240'000);
  std::this_thread::sleep_for(10ms);
  REQUIRE(f.video->timestamps().size()==stopped_ticks);
  REQUIRE(completion.wait_for(0ms)==std::future_status::timeout);
  f.muxer->release();
  const auto result=completion.get();
  REQUIRE(result.stopped.is_success());
  REQUIRE(result.saved.is_success());
  REQUIRE(result.metrics.video_ticks==stopped_ticks);
}
TEST_CASE("pipeline failure emits terminal error and disables replay") {
  class BrokenVideo final:public IEngineVideoPipeline {
    Result<std::vector<EncodedPacket>> tick(QpcTicks,bool) override {
      return Result<std::vector<EncodedPacket>>::failure({"capture.broken","Lost capture",{}});
    }
    Result<std::vector<EncodedPacket>> flush() override {
      return Result<std::vector<EncodedPacket>>::success({});
    }
  };
  Fixture f; auto d=f.dependencies(); d.video=std::make_shared<BrokenVideo>();
  SessionController c(std::move(d)); REQUIRE(c.start(f.config()).is_success());
  for(int n=0;n<500 && c.snapshot().lifecycle!=EngineLifecycle::Failed;++n) {
    std::this_thread::sleep_for(1ms); c.poll();
  }
  REQUIRE(c.snapshot().lifecycle==EngineLifecycle::Failed);
  REQUIRE_FALSE(c.snapshot().replay_active);
  bool fatal=false;
  for(const auto& e:c.take_events()) if(e.type==SessionEventType::FatalError) {
    REQUIRE(e.error->code=="capture.broken"); fatal=true;
  }
  REQUIRE(fatal);
}
TEST_CASE("host configuration rejects missing pinned IDs and unbounded test durations") {
  const std::string config=R"({"protocolVersion":1,"monitorId":"1:0","systemAudioEnabled":false,"systemAudioId":"","microphoneEnabled":false,"microphoneId":"","width":1920,"height":1080,"fps":60,"bitrate":30000000,"replaySeconds":30,"clipSeconds":30,"container":"mp4","outputDirectory":"C:\\clips","saveReplayHotkey":"F8","toggleRecordingHotkey":"Ctrl+Shift+R","continuousRecordingEnabled":false})";
  REQUIRE(decode_host_configuration(config,false).is_success());
  auto enabled=config; enabled.replace(enabled.find("Enabled\":false"),14,"Enabled\":true");
  REQUIRE_FALSE(decode_host_configuration(enabled,false).is_success());
  const std::string test_config=R"({"protocolVersion":1,"monitorId":"1:0","systemAudioEnabled":false,"systemAudioId":"","microphoneEnabled":false,"microphoneId":"","width":1920,"height":1080,"fps":60,"bitrate":30000000,"replaySeconds":30,"clipSeconds":30,"container":"mp4","outputDirectory":"C:\\clips","saveReplayHotkey":"F8","toggleRecordingHotkey":"Ctrl+Shift+R","continuousRecordingEnabled":false,"durationSeconds":5,"outputPath":"C:\\clips\\test.mp4"})";
  REQUIRE(decode_host_configuration(test_config,true).is_success());
  for(const auto duration:{0,4,6}) {
    auto rejected=test_config;
    rejected.replace(rejected.find("durationSeconds\":5")+17,1,std::to_string(duration));
    REQUIRE_FALSE(decode_host_configuration(rejected,true).is_success());
  }
}

TEST_CASE("native recording bitrate agrees with desktop limits") {
  const std::string config=R"({"protocolVersion":1,"monitorId":"1:0","systemAudioEnabled":false,"systemAudioId":"","microphoneEnabled":false,"microphoneId":"","width":1920,"height":1080,"fps":60,"bitrate":30000000,"replaySeconds":30,"clipSeconds":30,"container":"mp4","outputDirectory":"C:\\clips","saveReplayHotkey":"Alt+F10","toggleRecordingHotkey":"Ctrl+Shift+R","continuousRecordingEnabled":false})";
  for (const auto bitrate : {999999U, 200000001U}) {
    auto rejected = config;
    rejected.replace(rejected.find("30000000"), 8, std::to_string(bitrate));
    REQUIRE_FALSE(decode_host_configuration(rejected, false).is_success());
  }
}

TEST_CASE("recording contract boundaries messages and defaults agree with the native host") {
  using nlohmann::json;
  auto config = json::parse(R"({"protocolVersion":1,"monitorId":"1:0","systemAudioEnabled":false,"systemAudioId":"","microphoneEnabled":false,"microphoneId":"","width":1920,"height":1080,"fps":60,"bitrate":30000000,"replaySeconds":30,"clipSeconds":1,"container":"mp4","outputDirectory":"C:\\clips","saveReplayHotkey":"Alt+F10","toggleRecordingHotkey":"Ctrl+Shift+R","continuousRecordingEnabled":false})");
  REQUIRE(recording_contract().at("version") == 1);
  for (const auto& [field, range] : recording_contract().at("ranges").items()) {
    const auto min = range.at("min").get<std::uint32_t>();
    const auto max = range.at("max").get<std::uint32_t>();
    for (const auto scalar : {min - 1, min, max, max + 1}) {
      auto candidate = config;
      candidate[field == "replay_seconds" ? "replaySeconds" : field == "replay_memory_limit_mb" ? "replayMemoryLimitMb" : field] = scalar;
      const auto result = decode_host_configuration(candidate.dump(), false);
      const bool accepted = scalar >= min && scalar <= max;
      INFO(field << "=" << scalar);
      REQUIRE(result.is_success() == accepted);
      if (!accepted) REQUIRE(result.error().message == recording_contract().at("messages").at(field).get<std::string>());
    }
  }
  const EngineConfig defaults;
  REQUIRE(defaults.save_replay_hotkey == recording_default_hotkey("save_replay_hotkey"));
  REQUIRE(format_hotkey_chord(defaults.save_replay_hotkey) == L"Alt+F10");
  REQUIRE(format_hotkey_chord(defaults.toggle_recording_hotkey) == L"Ctrl+Shift+R");
}

TEST_CASE("host configuration accepts optional replay memory without relaxing strict keys") {
  using nlohmann::json;
  const auto legacy=json::parse(R"({"protocolVersion":1,"monitorId":"1:0","systemAudioEnabled":false,"systemAudioId":"","microphoneEnabled":false,"microphoneId":"","width":1920,"height":1080,"fps":60,"bitrate":30000000,"replaySeconds":30,"clipSeconds":1,"container":"mp4","outputDirectory":"C:\\clips","saveReplayHotkey":"Alt+F10","toggleRecordingHotkey":"Ctrl+Shift+R","continuousRecordingEnabled":false})");
  for(const bool test:{false,true}) {
    auto base=legacy;
    if(test){base["durationSeconds"]=5;base["outputPath"]="C:/clips/test.mp4";base["outputDirectory"]="C:/clips";}
    const auto old=decode_host_configuration(base.dump(),test);
    REQUIRE(old.is_success());REQUIRE(old.value().engine.replay_memory_limit_mb==0);
    for(const auto limit:{0U,64U,512U,8192U}) {
      auto configured=base;configured["replayMemoryLimitMb"]=limit;
      const auto decoded=decode_host_configuration(configured.dump(),test);
      REQUIRE(decoded.is_success());REQUIRE(decoded.value().engine.replay_memory_limit_mb==limit);
    }
    for(const auto invalid:{json(-1),json(63),json(8193),json(128.5),json("512"),json(nullptr),json(true)}) {
      auto configured=base;configured["replayMemoryLimitMb"]=invalid;
      REQUIRE_FALSE(decode_host_configuration(configured.dump(),test).is_success());
    }
    auto unknown=base;unknown["unexpected"]=0;
    REQUIRE_FALSE(decode_host_configuration(unknown.dump(),test).is_success());
    auto missing=base;missing.erase("width");missing["replayMemoryLimitMb"]=512;
    REQUIRE_FALSE(decode_host_configuration(missing.dump(),test).is_success());
    auto duplicate=base.dump();duplicate.insert(1,"\"replayMemoryLimitMb\":512,\"replayMemoryLimitMb\":256,");
    REQUIRE_FALSE(decode_host_configuration(duplicate,test).is_success());
  }
}
