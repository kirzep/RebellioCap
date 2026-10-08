#include "config/recording_contract.h"
#include "engine/session_controller.h"
#include "engine/session_input.h"
#include "engine/hardware_pipeline_factory.h"
#include "platform/windows/com_apartment.h"
#include <Windows.h>
#include <array>
#include <fstream>
#include <thread>
#include <nlohmann/json.hpp>

namespace rebelliocap {
namespace {
using nlohmann::json;
using namespace std::chrono_literals;
Error error(std::string code, std::string message) { return {std::move(code),std::move(message),{}}; }
std::wstring wide(const std::string& text) {
  if(text.find('\0')!=std::string::npos) throw std::runtime_error("Embedded NUL");
  if(text.empty()) return {};
  int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
  if(count==0) throw std::runtime_error("Invalid UTF-8");
  std::wstring result(static_cast<std::size_t>(count),L'\0');
  MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count);
  return result;
}
std::string path_text(const std::filesystem::path& path) {
  auto text=path.u8string(); return {text.begin(),text.end()};
}
}

Result<HostConfiguration> decode_host_configuration(std::string_view text, bool test) {
  try {
    if(text.size()>kMaximumSessionMessageBytes) throw std::runtime_error("Configuration exceeds 64 KiB");
    bool duplicate=false;
    std::unordered_set<std::string> keys;
    auto value=json::parse(text, [&](int depth,json::parse_event_t event,json& item) {
      if(depth>2) throw std::runtime_error("Nested configuration is not supported");
      if(event==json::parse_event_t::key && !keys.insert(item.get<std::string>()).second) duplicate=true;
      return true;
    });
    if(duplicate || !value.is_object() || value.size()!=(test?19U:17U)+(value.contains("replayMemoryLimitMb")?1U:0U))
      throw std::runtime_error("Configuration fields are missing, duplicated, or unrecognized");
    if(!value.at("protocolVersion").is_number_unsigned() || value.at("protocolVersion")!=1)
      throw std::runtime_error("Unsupported configuration protocol version");
    auto number=[&](const char* key,std::uint32_t low,std::uint32_t high) {
      const auto& field=value.at(key);
      if(!field.is_number_unsigned() || field.get<std::uint64_t>()<low || field.get<std::uint64_t>()>high)
        throw std::runtime_error(std::string("Invalid ")+key);
      return field.get<std::uint32_t>();
    };
    HostConfiguration result;
    auto& c=result.engine;
    c.system_audio_enabled=value.at("systemAudioEnabled").get<bool>();
    c.microphone_enabled=value.at("microphoneEnabled").get<bool>();
    c.system_audio_id=wide(value.at("systemAudioId").get<std::string>());
    c.microphone_id=wide(value.at("microphoneId").get<std::string>());
    if((c.system_audio_enabled && c.system_audio_id.empty()) || (c.microphone_enabled && c.microphone_id.empty()))
      throw std::runtime_error("Enabled audio requires an explicit pinned endpoint ID");
    auto recording_number = [&](const char* key, const char* field) {
      const auto& range = recording_contract().at("ranges").at(field);
      const auto& scalar = value.at(key);
      if (!scalar.is_number_unsigned() || scalar.get<std::uint64_t>() < range.at("min").get<std::uint32_t>() || scalar.get<std::uint64_t>() > range.at("max").get<std::uint32_t>())
        throw std::runtime_error(recording_contract().at("messages").at(field).get<std::string>());
      return scalar.get<std::uint32_t>();
    };
    if(value.contains("replayMemoryLimitMb")) {
      const auto& limit=value.at("replayMemoryLimitMb");
      if(!limit.is_number_unsigned()) throw std::runtime_error(recording_contract().at("messages").at("replay_memory_limit_mb").get<std::string>());
      c.replay_memory_limit_mb=limit==0?0:recording_number("replayMemoryLimitMb","replay_memory_limit_mb");
    }
    c.width=recording_number("width","width"); c.height=recording_number("height","height");
    if(c.width%2) throw std::runtime_error(recording_contract().at("messages").at("width").get<std::string>());
    if(c.height%2) throw std::runtime_error(recording_contract().at("messages").at("height").get<std::string>());
    c.fps=recording_number("fps","fps"); c.bitrate=recording_number("bitrate","bitrate");
    c.replay_capacity=std::chrono::seconds(recording_number("replaySeconds","replay_seconds"));
    c.clip_duration=std::chrono::seconds(recording_number("clipSeconds","replay_seconds"));
    if(c.clip_duration>c.replay_capacity) throw std::runtime_error("Clip exceeds replay capacity");
    const auto container=value.at("container").get<std::string>();
    if(container!="mp4" && container!="mkv") throw std::runtime_error("Unsupported container");
    c.container=container=="mp4"?Container::Mp4:Container::Mkv;
    c.output_directory=wide(value.at("outputDirectory").get<std::string>());
    if(!c.output_directory.is_absolute()) throw std::runtime_error("Output directory must be absolute");
    auto save=parse_hotkey_chord(wide(value.at("saveReplayHotkey").get<std::string>()));
    auto toggle=parse_hotkey_chord(wide(value.at("toggleRecordingHotkey").get<std::string>()));
    if(!save.is_success()) return Result<HostConfiguration>::failure(save.error());
    if(!toggle.is_success()) return Result<HostConfiguration>::failure(toggle.error());
    c.save_replay_hotkey=save.value(); c.toggle_recording_hotkey=toggle.value();
    if(save.value()==toggle.value()) throw std::runtime_error("Hotkeys must be distinct");
    c.continuous_recording_enabled=value.at("continuousRecordingEnabled").get<bool>();
    auto monitor=wide(value.at("monitorId").get<std::string>());
    const std::array<std::wstring_view,5> args{L"capture",L"--monitor",monitor,L"--output",L"unused"};
    auto parsed=parse_arguments(args);
    if(!parsed.is_success()) return Result<HostConfiguration>::failure(parsed.error());
    c.monitor=*parsed.value().monitor;
    if(test) {
      result.duration_seconds=number("durationSeconds",5,5);
      result.test_output=wide(value.at("outputPath").get<std::string>());
      if(!result.test_output->is_absolute() || result.test_output->parent_path()!=c.output_directory ||
          result.test_output->extension()!=(c.container==Container::Mp4?L".mp4":L".mkv"))
        throw std::runtime_error("Test output must be an absolute matching container path in outputDirectory");
      c.continuous_recording_enabled=false;
      c.replay_capacity=7s; c.clip_duration=5s;
    }
    return Result<HostConfiguration>::success(std::move(result));
  } catch(const std::exception& e) {
    return Result<HostConfiguration>::failure(error("config.invalid",e.what()));
  }
}

SessionController::SessionController(RecorderEngineDependencies dependencies)
    :clock_(dependencies.clock),engine_(std::move(dependencies)) {}
void SessionController::refresh() {
  snapshot_.metrics=engine_.metrics();
  snapshot_.continuous_recording_active=snapshot_.metrics.continuous_recording_active;
  snapshot_.replay_active=engine_.replay_enabled();
  ++snapshot_.revision;
}
void SessionController::publish(SessionEventType type,std::optional<std::string> id) {
  events_.push_back({type,std::move(id),snapshot_,{}, {}});
}
void SessionController::fail(const Error& failure) {
  snapshot_.lifecycle=EngineLifecycle::Failed; snapshot_.replay_active=false;
  snapshot_.last_error=failure; refresh();
  events_.push_back({SessionEventType::FatalError,{},snapshot_,failure,{}});
  publish(SessionEventType::Snapshot);
}
Result<void> SessionController::start(const EngineConfig& config) {
  if(started_ || snapshot_.revision!=0) return Result<void>::failure(error("session.already_started","Session cannot be restarted"));
  auto result=engine_.start(config);
  if(!result.is_success()) { fail(result.error()); return result; }
  started_=true; snapshot_.lifecycle=EngineLifecycle::Ready; snapshot_.replay_active=true;
  snapshot_.replay_seconds=static_cast<std::uint32_t>(config.replay_capacity.count());
  refresh(); publish(SessionEventType::Ready); publish(SessionEventType::Snapshot);
  return Result<void>::success();
}
Result<void> SessionController::stop() {
  if(!started_) return Result<void>::success();
  started_=false;
  auto result=engine_.stop();
  if(!result.is_success()) { fail(result.error()); return result; }
  snapshot_.lifecycle=EngineLifecycle::Stopped; snapshot_.replay_active=false;
  refresh(); publish(SessionEventType::Snapshot); poll();
  return result;
}
Result<void> SessionController::handle(const SessionRequest& request) {
  auto result=Result<void>::success();
  if(request_ids_.contains(request.request_id)) {
    result=Result<void>::failure(error("protocol.duplicate_request_id","Request ID has already been used"));
  } else if(request_ids_.size()>=65536) {
    result=Result<void>::failure(error("protocol.request_limit","Session request ID capacity reached"));
    static_cast<void>(stop()); fail(result.error());
  } else {
    request_ids_.insert(request.request_id);
    if(request.command==SessionCommand::Stop) result=stop();
    else if(request.command==SessionCommand::GetSnapshot) { refresh(); publish(SessionEventType::Snapshot,request.request_id); }
    else if(!started_) result=Result<void>::failure(error("engine.not_running","Session is stopped"));
    else if(request.command==SessionCommand::ReloadRecordingNames) result=engine_.reload_recording_names();
    else if(request.command==SessionCommand::StartReplay || request.command==SessionCommand::StopReplay) {
      result=engine_.set_replay_enabled(request.command==SessionCommand::StartReplay);
      refresh(); publish(SessionEventType::Snapshot,request.request_id);
    }
    else if(request.command==SessionCommand::ToggleRecording) {
      result=engine_.toggle_continuous_recording(); refresh(); publish(SessionEventType::Snapshot,request.request_id);
    } else {
      if(!engine_.replay_enabled()) result=Result<void>::failure(error("engine.replay_disabled","Replay is disabled"));
      else if(saves_.size()>=16) result=Result<void>::failure(error("session.save_queue_full","Pending save limit reached"));
      else { saves_.push_back({request.request_id,engine_.save_clip(clock_->now())}); refresh(); publish(SessionEventType::Snapshot,request.request_id); }
    }
  }
  events_.push_back({SessionEventType::CommandResult,request.request_id,snapshot_,
      result.is_success()?std::optional<Error>{}:result.error(),{}});
  return result;
}
void SessionController::poll(bool publish_snapshot) {
  auto metrics=engine_.metrics();
  if(started_ && metrics.pipeline_errors>0) { static_cast<void>(stop()); return; }
  if(metrics.continuous_recording_active!=snapshot_.continuous_recording_active ||
     metrics.audio_unavailable_sources!=snapshot_.metrics.audio_unavailable_sources ||
     metrics.replay_memory_limited!=snapshot_.metrics.replay_memory_limited ||
     metrics.continuous_failures!=snapshot_.metrics.continuous_failures ||
     metrics.continuous_recovery_path!=snapshot_.metrics.continuous_recovery_path ||
     metrics.completed_saves!=snapshot_.metrics.completed_saves ||
     metrics.failed_saves!=snapshot_.metrics.failed_saves) publish_snapshot=true;
  for(auto& completion:engine_.take_hotkey_save_completions()) {
    refresh();
    events_.push_back({completion.output_path.has_value()?SessionEventType::ClipSaved:SessionEventType::Error,
      {},snapshot_,std::move(completion.error),std::move(completion.output_path)});
    publish_snapshot=true;
  }
  for(auto it=saves_.begin();it!=saves_.end();) {
    if(it->future.wait_for(0ms)!=std::future_status::ready) { ++it; continue; }
    auto result=it->future.get(); refresh();
    events_.push_back({result.is_success()?SessionEventType::ClipSaved:SessionEventType::Error,
      it->id,snapshot_,result.is_success()?std::optional<Error>{}:result.error(),
      result.is_success()?std::optional<std::filesystem::path>{result.value()}:std::nullopt});
    it=saves_.erase(it); publish_snapshot=true;
  }
  if(publish_snapshot) { refresh(); publish(SessionEventType::Snapshot); }
}
std::vector<SessionEvent> SessionController::take_events() { return std::exchange(events_,{}); }

BoundedRecordingResult finish_bounded_recording(RecorderEngine& engine,
                                                 QpcTicks requested_at) {
  auto saved = engine.save_clip(requested_at);
  auto stopped = engine.stop();
  auto saved_result = saved.get();
  return {.saved = std::move(saved_result),
          .stopped = std::move(stopped),
          .metrics = engine.metrics()};
}

CliExitCode run_native_host(const CliArguments& arguments,std::ostream& output) {
  auto fatal=[&](const Error& failure) {
    output<<encode_session_event({SessionEventType::FatalError,{},{},failure,{}})<<std::flush;
    return CliExitCode::CaptureFailure;
  };
  try {
    ComApartment apartment;
    if(!apartment.initialized()) return fatal(error("engine.com_initialization_failed","COM initialization failed"));
    HardwarePipelineFactory factory;
    if(arguments.command==CliCommand::CatalogJson) {
      auto result=factory.catalog(); if(!result.is_success()) return fatal(result.error());
      output<<result.value().dump()<<'\n'<<std::flush; return CliExitCode::Success;
    }
    if(arguments.command==CliCommand::DoctorJson) {
      auto result=factory.doctor(arguments.monitor); output<<result.dump()<<'\n'<<std::flush;
      return result.at("passed").get<bool>()?CliExitCode::Success:CliExitCode::UnsupportedHardware;
    }
    std::ifstream file(arguments.config_path,std::ios::binary);
    if(!file) return fatal(error("config.read_failed","Cannot open host configuration"));
    std::array<char,kMaximumSessionMessageBytes+1> buffer{};
    file.read(buffer.data(),static_cast<std::streamsize>(buffer.size()));
    const bool test=arguments.command==CliCommand::TestJson;
    auto config=decode_host_configuration({buffer.data(),static_cast<std::size_t>(file.gcount())},test);
    if(!config.is_success()) return fatal(config.error());
    auto& host=config.value();
    if(!test)host.engine.naming_settings_file=arguments.config_path.parent_path()/L"recording-names.json";
    std::error_code ec;
    std::filesystem::create_directories(host.engine.output_directory,ec);
    if(ec) return fatal(error("mux.output_directory_failed",ec.message()));
    if(test && std::filesystem::exists(*host.test_output)) return fatal(error("mux.destination_exists","Test output already exists"));
    auto dependencies=factory.create(host.engine);
    if(!dependencies.is_success()) return fatal(dependencies.error());
    if(test) {
      // Onboarding must publish its exact owned test path, outside game categories.
      dependencies.value().capture_category = {};
      auto clock=dependencies.value().clock;
      RecorderEngine engine(std::move(dependencies).value());
      auto start=engine.start(host.engine); if(!start.is_success()) return fatal(start.error());
      std::this_thread::sleep_for(5s);
      auto completed=finish_bounded_recording(engine,clock->now());
      if(!completed.stopped.is_success()) return fatal(completed.stopped.error());
      if(!completed.saved.is_success()) return fatal(completed.saved.error());
      if(!MoveFileExW(completed.saved.value().c_str(),host.test_output->c_str(),MOVEFILE_WRITE_THROUGH))
        return fatal(error("mux.final_rename_failed","Could not publish exact test output path"));
      const auto size=std::filesystem::file_size(*host.test_output,ec);
      const auto& metrics=completed.metrics;
      if(ec || size==0 || metrics.video_packets==0 || metrics.completed_saves!=1)
        return fatal(error("mux.validation_failed","Final test recording failed validation"));
      auto encoded=json::parse(encode_session_event({SessionEventType::Snapshot,{},
          EngineSnapshot{1,EngineLifecycle::Stopped,false,false,5,metrics,{}},{},{}}));
      output<<json{{"protocolVersion",1},{"type","test_result"},{"passed",true},
        {"initializationStages",{"configuration","monitor","nvenc","audio","recorder","capture","mux","file_validation"}},
        {"durationSeconds",5},{"videoPackets",metrics.video_packets},{"audioPackets",metrics.audio_packets},
        {"missedFrames",metrics.missed_video_deadlines},{"droppedFrames",metrics.missed_video_deadlines},
        {"muxCompleted",true},{"fileSize",size},{"outputPath",path_text(*host.test_output)},
        {"metrics",encoded.at("snapshot").at("metrics")}}.dump()<<'\n'<<std::flush;
      return CliExitCode::Success;
    }
    auto wake = std::make_shared<SessionWake>();
    dependencies.value().state_changed = [wake] { wake->notify(); };
    SessionController controller(std::move(dependencies).value());
    auto flush=[&] { for(const auto& event:controller.take_events()) output<<encode_session_event(event); output.flush(); };
    auto started=controller.start(host.engine); flush(); if(!started.is_success()) return CliExitCode::CaptureFailure;
    const HANDLE input=GetStdHandle(STD_INPUT_HANDLE);
    if(GetFileType(input)!=FILE_TYPE_PIPE) { static_cast<void>(controller.stop()); flush(); return fatal(error("protocol.pipe_required","Session input must be an inherited pipe")); }
    SessionPipeReader reader(input, wake);
    std::string line;
    bool too_large=false, eof=false;
    auto next_snapshot=std::chrono::steady_clock::now()+1s;
    while(!eof && controller.snapshot().lifecycle==EngineLifecycle::Ready && output.good()) {
      const auto revision = wake->revision();
      if(auto chunk = reader.try_take()) {
        if(chunk->error) { static_cast<void>(controller.stop()); flush(); return fatal(*chunk->error); }
        eof = chunk->eof;
        const auto& bytes = chunk->bytes;
        for(std::size_t i=0;i<bytes.size();++i) {
          if(bytes[i]=='\n') {
            if(too_large) output<<encode_session_event({SessionEventType::Error,{},{},error("protocol.message_too_large","Request exceeds 64 KiB"),{}});
            else {
              std::optional<std::string> rejected_request_id;
              auto request=decode_session_request(line,&rejected_request_id);
              if(request.is_success()) static_cast<void>(controller.handle(request.value()));
              else output<<encode_session_event({SessionEventType::Error,rejected_request_id,{},request.error(),{}});
            }
            line.clear(); too_large=false; flush();
            if(controller.snapshot().lifecycle!=EngineLifecycle::Ready) break;
          } else if(!too_large) {
            if(line.size()==kMaximumSessionMessageBytes) { line.clear(); too_large=true; }
            else line.push_back(bytes[i]);
          }
        }
      }
      const auto now=std::chrono::steady_clock::now();
      controller.poll(now>=next_snapshot);
      if(now>=next_snapshot) next_snapshot=now+1s;
      flush();
      if(!eof && reader.queued_chunks()==0 && controller.snapshot().lifecycle==EngineLifecycle::Ready && output.good()) {
        static_cast<void>(wake->wait_until(revision, next_snapshot));
      }
    }
    if(eof && (!line.empty() || too_large)) output<<encode_session_event({SessionEventType::Error,{},{},error("protocol.truncated_message","EOF before request newline"),{}});
    auto stopped=controller.stop(); controller.poll(); flush();
    return stopped.is_success() && controller.snapshot().lifecycle!=EngineLifecycle::Failed?CliExitCode::Success:CliExitCode::CaptureFailure;
  } catch(const std::exception& e) { return fatal(error("session.exception",e.what())); }
}
}
