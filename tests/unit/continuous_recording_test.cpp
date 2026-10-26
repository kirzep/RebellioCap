#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <thread>
#include <atomic>
#include <array>
#include <barrier>
#include "engine/continuous_recording.h"
#include "engine/session_input.h"
#include "fakes/fake_continuous_muxer.h"
#include "mux/ffmpeg_continuous_muxer.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
using namespace rebelliocap;

TEST_CASE("audio track names survive MP4 and MKV publication") {
  for (const auto container : {Container::Mp4, Container::Mkv}) {
    const auto destination = std::filesystem::temp_directory_path() /
        ("RebellioCap-track-names-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         (container == Container::Mp4 ? ".mp4" : ".mkv"));
    std::vector<StreamDescriptor> descriptors;
    for (const auto kind : {StreamKind::MixedAudio, StreamKind::SystemAudio,
                            StreamKind::MicrophoneAudio}) {
      descriptors.push_back({kind, "aac", "Legacy name",
          {std::byte{0x11}, std::byte{0x90}},
          {1, static_cast<std::int32_t>(QpcClock{}.frequency())}, 0, 0, 48'000, 2});
    }
    FfmpegContinuousMuxer muxer;
    REQUIRE(muxer.open(descriptors, destination, container).is_success());
    const auto payload = std::make_shared<const std::vector<std::byte>>(
        std::initializer_list<std::byte>{std::byte{0x21}, std::byte{0x10},
                                        std::byte{0x04}, std::byte{0x60},
                                        std::byte{0x8c}, std::byte{0x1c}});
    for (const auto& descriptor : descriptors) {
      REQUIRE(muxer.write({descriptor.kind, 1, 0, 0,
          QpcClock{}.frequency() * 1024 / 48'000, false, payload}).is_success());
    }
    REQUIRE(muxer.finalize().is_success());
    AVFormatContext* input = nullptr;
    REQUIRE(avformat_open_input(&input, destination.string().c_str(), nullptr, nullptr) == 0);
    REQUIRE(input->nb_streams == 3);
    const char* expected[]{"Системный звук + микрофон", "Системный звук", "Микрофон"};
    for (unsigned i = 0; i < input->nb_streams; ++i) {
      const auto* name = av_dict_get(input->streams[i]->metadata,
          container == Container::Mp4 ? "handler_name" : "title", nullptr, 0);
      REQUIRE(name != nullptr);
      CHECK(std::string(name->value) == expected[i]);
    }
    avformat_close_input(&input);
    REQUIRE(std::filesystem::remove(destination));
  }
}
namespace {
EncodedPacket packet(QpcTicks time, StreamKind kind = StreamKind::Video) {
  return {kind, 1, time, time, 1, true, std::make_shared<const std::vector<std::byte>>(4)};
}
}
TEST_CASE("continuous recording drains and finalizes accepted video and audio") {
  FakeContinuousMuxer muxer;
  ContinuousRecording recording(muxer, {});
  REQUIRE(recording.start(L"clips/recording-000001.mp4", Container::Mp4).is_success());
  REQUIRE(recording.accept(packet(10)).is_success());
  REQUIRE(recording.accept(packet(11, StreamKind::SystemAudio)).is_success());
  REQUIRE(recording.stop().is_success());
  REQUIRE(muxer.finished);
  REQUIRE(muxer.packets == 2);
  REQUIRE(recording.packets_written() == 2);
  REQUIRE_FALSE(recording.active());
}
TEST_CASE("continuous writer failure signals readiness before the host heartbeat") {
  FakeContinuousMuxer muxer; muxer.blocked=true; muxer.fail=true;
  auto wake=std::make_shared<SessionWake>();
  ContinuousRecording recording(muxer, {}, std::make_shared<PacketMemoryBudget>(8192), [wake]{wake->notify();});
  REQUIRE(recording.start(L"event-failure.mp4",Container::Mp4).is_success());
  REQUIRE(recording.accept(packet(0)).is_success());REQUIRE(muxer.wait_entered());
  const auto before=wake->revision();muxer.release();
  REQUIRE(wake->wait_until(before,std::chrono::steady_clock::now()+std::chrono::seconds(2)));
  REQUIRE_FALSE(recording.active());REQUIRE(recording.failures()==1);
  REQUIRE_FALSE(recording.stop().is_success());
}

TEST_CASE("slow continuous writer is bounded by bytes before packet count", "[memory-budget]") {
  FakeContinuousMuxer muxer;
  muxer.blocked = true;
  ContinuousRecording recording(muxer, {});
  REQUIRE(recording.start(L"byte-bounded.mp4", Container::Mp4).is_success());
  auto first = packet(0);
  first.payload = std::make_shared<const std::vector<std::byte>>(4 * 1024 * 1024);
  const auto initial = recording.accept(std::move(first));
  const bool entered = muxer.wait_entered();
  bool rejected = false;
  std::string code;
  for (int i = 1; i < 20 && !rejected; ++i) {
    auto item = packet(i);
    item.payload = std::make_shared<const std::vector<std::byte>>(4 * 1024 * 1024);
    auto accepted = recording.accept(std::move(item));
    if (!accepted.is_success()) { rejected = true; code = accepted.error().code; }
  }
  muxer.release();
  const auto stopped = recording.stop();
  REQUIRE(initial.is_success());
  REQUIRE(entered);
  REQUIRE(rejected);
  REQUIRE(code == "recording.memory_budget_exceeded");
  REQUIRE_FALSE(stopped.is_success());
}

TEST_CASE("continuous writer and Replay share allocations and retain in flight charge", "[memory-budget]") {
  auto budget = std::make_shared<PacketMemoryBudget>(8192);
  FakeContinuousMuxer muxer;
  muxer.blocked = true;
  ContinuousRecording recording(muxer, {}, budget);
  REQUIRE(recording.start(L"shared-budget.mp4", Container::Mp4).is_success());
  auto item = packet(0);
  item.payload = std::make_shared<const std::vector<std::byte>>(4096);
  std::size_t ring_charge = 0;
  bool accepted = false;
  bool entered = false;
  {
    ReplayRing ring(30, budget);
    REQUIRE(ring.append(item).is_success());
    ring_charge = budget->bytes();
    accepted = recording.accept(item).is_success();
    entered = muxer.wait_entered();
  }
  const auto writer_charge = budget->bytes();
  muxer.release();
  const auto stopped = recording.stop();
  REQUIRE(accepted);
  REQUIRE(entered);
  REQUIRE(writer_charge == ring_charge);
  REQUIRE(stopped.is_success());
  REQUIRE(budget->bytes() == 0);
}
TEST_CASE("continuous recording queue overflow is bounded and stable") {
  FakeContinuousMuxer muxer;
  muxer.blocked = true;
  ContinuousRecording recording(muxer, {});
  REQUIRE(recording.start(L"bounded.mp4", Container::Mp4).is_success());
  REQUIRE(recording.accept(packet(0)).is_success());
  const auto entered = muxer.wait_entered();
  if (!entered) muxer.release();
  REQUIRE(entered);
  bool accepted = true;
  for (int i = 0; i < 4096; ++i) accepted &= recording.accept(packet(i + 1)).is_success();
  const auto overflow = recording.accept(packet(4097));
  muxer.release();
  REQUIRE(accepted);
  REQUIRE_FALSE(overflow.is_success());
  REQUIRE(overflow.error().code == "recording.queue_full");
  REQUIRE(recording.stop().error().code == "recording.queue_full");
  REQUIRE(recording.accept(packet(5000)).error().code == "recording.queue_full");
  REQUIRE(muxer.aborted);
  REQUIRE_FALSE(muxer.finished);
}
TEST_CASE("continuous recording preserves the first writer failure") {
  FakeContinuousMuxer muxer;
  muxer.fail = true;
  ContinuousRecording recording(muxer, {});
  REQUIRE(recording.start(L"failure.mp4", Container::Mp4).is_success());
  REQUIRE(recording.accept(packet(10)).is_success());
  const auto result = recording.stop();
  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "mux.injected");
  REQUIRE(recording.stop().error().code == "mux.injected");
  REQUIRE_FALSE(recording.active());
  REQUIRE(muxer.aborted);
}

TEST_CASE("continuous recording starts video at a keyframe when enabled mid GOP") {
  FakeContinuousMuxer muxer;
  ContinuousRecording recording(muxer, {StreamDescriptor{.kind = StreamKind::Video}});
  REQUIRE(recording.start(L"mid-gop.mp4", Container::Mp4).is_success());
  auto dependent = packet(10);
  dependent.keyframe = false;
  REQUIRE(recording.accept(dependent).is_success());
  REQUIRE(recording.accept(packet(11, StreamKind::SystemAudio)).is_success());
  REQUIRE(recording.accept(packet(12)).is_success());
  REQUIRE(recording.accept(packet(13, StreamKind::SystemAudio)).is_success());
  REQUIRE(recording.stop().is_success());
  REQUIRE(recording.packets_written() == 2);
}

TEST_CASE("continuous recording stop before a usable frame aborts with stable no media") {
  FakeContinuousMuxer muxer;
  ContinuousRecording recording(muxer, {StreamDescriptor{.kind = StreamKind::Video}});
  REQUIRE(recording.start(L"no-media.mp4", Container::Mp4).is_success());
  SECTION("stopped before any packet arrives") {}
  SECTION("audio and dependent video arrive without a keyframe") {
    REQUIRE(recording.accept(packet(10, StreamKind::SystemAudio)).is_success());
    auto dependent = packet(11);
    dependent.keyframe = false;
    REQUIRE(recording.accept(dependent).is_success());
  }
  const auto stopped = recording.stop();
  REQUIRE_FALSE(stopped.is_success());
  REQUIRE(stopped.error().code == "recording.no_media");
  REQUIRE(recording.stop().error().code == "recording.no_media");
  REQUIRE(recording.accept(packet(12)).error().code == "recording.no_media");
  REQUIRE(recording.packets_written() == 0);
  REQUIRE(recording.failures() == 1);
  REQUIRE_FALSE(recording.active());
  REQUIRE(muxer.aborted);
  REQUIRE_FALSE(muxer.finished);
}

TEST_CASE("continuous recording refuses an existing destination without starting a writer") {
  FakeContinuousMuxer muxer;
  muxer.open_error = Error{"mux.destination_exists", "Destination exists", {}};
  ContinuousRecording recording(muxer, {});

  const auto result = recording.start(L"existing.mp4", Container::Mp4);

  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "mux.destination_exists");
  REQUIRE_FALSE(recording.active());
  REQUIRE_FALSE(muxer.opened);
  REQUIRE_FALSE(muxer.aborted);
}

TEST_CASE("FFmpeg continuous mux never overwrites an existing destination") {
  const auto directory = std::filesystem::temp_directory_path() /
      ("RebellioCap-continuous-collision-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directory(directory));
  const auto destination = directory / L"existing.mp4";
  {
    std::ofstream output(destination, std::ios::binary);
    output << "sentinel";
  }
  FfmpegContinuousMuxer muxer;

  const auto result = muxer.open({}, destination, Container::Mp4);

  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "mux.destination_exists");
  std::ifstream input(destination, std::ios::binary);
  std::string contents;
  input >> contents;
  REQUIRE(contents == "sentinel");
  input.close();
  REQUIRE(std::filesystem::remove(destination));
  REQUIRE(std::filesystem::remove(directory));
}

TEST_CASE("FFmpeg continuous mux abort removes only its owned partial file") {
  const auto directory = std::filesystem::temp_directory_path() /
      ("RebellioCap-continuous-abort-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directory(directory));
  const auto destination = directory / L"aborted.mp4";
  const auto partial = std::filesystem::path(destination.wstring() + L".partial");
  const std::vector descriptors{StreamDescriptor{
      StreamKind::SystemAudio, "aac", "System",
      {std::byte{0x11}, std::byte{0x90}},
      {1, static_cast<std::int32_t>(QpcClock{}.frequency())},
      0, 0, 48'000, 2}};
  FfmpegContinuousMuxer muxer;

  REQUIRE(muxer.open(descriptors, destination, Container::Mp4).is_success());
  REQUIRE(std::filesystem::exists(partial));
  muxer.abort();

  REQUIRE_FALSE(std::filesystem::exists(partial));
  REQUIRE_FALSE(std::filesystem::exists(destination));
  REQUIRE(std::filesystem::remove(directory));
}

TEST_CASE("FFmpeg continuous mux preserves a foreign partial file") {
  const auto directory = std::filesystem::temp_directory_path() /
      ("RebellioCap-continuous-foreign-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directory(directory));
  const auto destination = directory / L"foreign.mp4";
  const auto partial = std::filesystem::path(destination.wstring() + L".partial");
  { std::ofstream output(partial); output << "foreign"; }
  FfmpegContinuousMuxer muxer;
  const auto opened = muxer.open({}, destination, Container::Mp4);
  REQUIRE_FALSE(opened.is_success());
  REQUIRE(opened.error().code == "mux.destination_exists");
  muxer.abort();
  std::string contents;
  { std::ifstream input(partial); input >> contents; }
  REQUIRE(contents == "foreign");
  REQUIRE(std::filesystem::remove(partial));
  REQUIRE(std::filesystem::remove(directory));
}

TEST_CASE("FFmpeg continuous publication preserves a destination created during recording") {
  const auto directory = std::filesystem::temp_directory_path() /
      ("RebellioCap-continuous-publish-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directory(directory));
  const auto destination = directory / L"collision.mp4";
  const auto partial = std::filesystem::path(destination.wstring() + L".partial");
  const std::vector descriptors{StreamDescriptor{
      StreamKind::SystemAudio, "aac", "System", {std::byte{0x11}, std::byte{0x90}},
      {1, static_cast<std::int32_t>(QpcClock{}.frequency())}, 0, 0, 48'000, 2}};
  FfmpegContinuousMuxer muxer;
  REQUIRE(muxer.open(descriptors, destination, Container::Mp4).is_success());
  { std::ofstream output(destination); output << "created-during-recording"; }
  const auto published = muxer.finalize();
  REQUIRE_FALSE(published.is_success());
  REQUIRE(published.error().code == "mux.destination_exists");
  std::string contents;
  { std::ifstream input(destination); input >> contents; }
  REQUIRE(contents == "created-during-recording");
  REQUIRE_FALSE(std::filesystem::exists(partial));
  REQUIRE(std::filesystem::remove(destination));
  REQUIRE(std::filesystem::remove(directory));
}

namespace {
std::vector<StreamDescriptor> recovery_audio_descriptors() {
  return {{StreamKind::SystemAudio, "aac", "System", {std::byte{0x11}, std::byte{0x90}},
           {1, 48000}, 0, 0, 48000, 2}};
}
void write_recovery_audio(FfmpegContinuousMuxer& muxer) {
  auto payload=std::make_shared<const std::vector<std::byte>>(std::initializer_list<std::byte>{
      std::byte{0x21},std::byte{0x10},std::byte{0x04},std::byte{0x60},std::byte{0x8c},std::byte{0x1c}});
  for(int i=0;i<150;++i)
    REQUIRE(muxer.write({StreamKind::SystemAudio,1,i*1024,i*1024,1024,true,payload}).is_success());
}
void require_decodable_recovery(const std::filesystem::path& path) {
  AVFormatContext* input=nullptr;
  const auto opened=avformat_open_input(&input,path.string().c_str(),nullptr,nullptr);
  REQUIRE(opened==0);
  auto* codec=avcodec_find_decoder(AV_CODEC_ID_AAC);
  auto* decoder=avcodec_alloc_context3(codec);
  REQUIRE(decoder);
  REQUIRE(avcodec_parameters_to_context(decoder,input->streams[0]->codecpar)==0);
  REQUIRE(avcodec_open2(decoder,codec,nullptr)==0);
  auto* packet=av_packet_alloc(); auto* frame=av_frame_alloc();
  int frames=0;
  while(av_read_frame(input,packet)>=0) {
    REQUIRE(avcodec_send_packet(decoder,packet)==0);
    while(avcodec_receive_frame(decoder,frame)==0) { ++frames; av_frame_unref(frame); }
    av_packet_unref(packet);
  }
  av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&decoder); avformat_close_input(&input);
  REQUIRE(frames>=90); // At least the first completed two-second fragment.
}
}

TEST_CASE("continuous MP4 retains decodable media after abort and can reopen a new destination") {
  const auto directory=std::filesystem::temp_directory_path()/
      ("RebellioCap-recovery-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directory(directory));
  const auto destination=directory/"failed.mp4";
  const auto partial=std::filesystem::path(destination.wstring()+L".partial");
  FfmpegContinuousMuxer muxer;
  REQUIRE(muxer.open(recovery_audio_descriptors(),destination,Container::Mp4).is_success());
  write_recovery_audio(muxer);
  muxer.abort();
  REQUIRE(std::filesystem::exists(partial));
  REQUIRE(muxer.recovery_path()==partial);
  require_decodable_recovery(partial);
  muxer.abort(); // Idempotent cleanup must not delete retained media.
  REQUIRE(std::filesystem::exists(partial));
  REQUIRE(muxer.open(recovery_audio_descriptors(),directory/"next.mp4",Container::Mp4).is_success());
  muxer.abort();
  REQUIRE(std::filesystem::exists(partial));
  std::filesystem::remove_all(directory);
}

TEST_CASE("continuous MP4 completed fragments decode without a trailer") {
  const auto directory=std::filesystem::temp_directory_path()/
      ("RebellioCap-interrupted-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directory(directory));
  const auto destination=directory/"active.mp4";
  const auto partial=std::filesystem::path(destination.wstring()+L".partial");
  FfmpegContinuousMuxer muxer;
  REQUIRE(muxer.open(recovery_audio_descriptors(),destination,Container::Mp4).is_success());
  write_recovery_audio(muxer);
  const auto interrupted=directory/"interrupted.mp4";
  REQUIRE(std::filesystem::copy_file(partial,interrupted)); // Snapshot before destructor/trailer.
  muxer.abort();
  require_decodable_recovery(interrupted);
  std::filesystem::remove_all(directory);
}

TEST_CASE("writer failure publishes the retained path after recovery completes") {
  const auto directory=std::filesystem::temp_directory_path()/
      ("RebellioCap-writer-recovery-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directory(directory));
  const auto destination=directory/"failed.mp4";
  FfmpegContinuousMuxer muxer;
  ContinuousRecording recording(muxer,recovery_audio_descriptors());
  REQUIRE(recording.start(destination,Container::Mp4).is_success());
  for(int i=0;i<150;++i) {
    auto input=packet(i*1024,StreamKind::SystemAudio);
    input.duration=1024;
    input.payload=std::make_shared<const std::vector<std::byte>>(std::initializer_list<std::byte>{
        std::byte{0x21},std::byte{0x10},std::byte{0x04},std::byte{0x60},std::byte{0x8c},std::byte{0x1c}});
    REQUIRE(recording.accept(input).is_success());
  }
  REQUIRE(recording.accept(packet(153600,StreamKind::MicrophoneAudio)).is_success()); // No descriptor: writer fails.
  const auto stopped=recording.stop();
  REQUIRE_FALSE(stopped.is_success());
  REQUIRE(stopped.error().code=="mux.invalid_stream");
  REQUIRE(recording.failures()==1);
  const auto partial=std::filesystem::path(destination.wstring()+L".partial");
  const auto utf8=partial.u8string();
  const std::string expected(utf8.begin(),utf8.end());
  REQUIRE(recording.recovery_path()==expected);
  REQUIRE(stopped.error().message.find(expected)!=std::string::npos);
  require_decodable_recovery(partial);
  std::filesystem::remove_all(directory);
}

TEST_CASE("continuous byte watermarks include the blocked writer and reject batches atomically", "[continuous-limits]") {
  FakeContinuousMuxer muxer; muxer.blocked = true;
  ContinuousQueueLimits limits; limits.hard_bytes = 16; limits.soft_bytes = 12;
  ContinuousRecording recording(muxer, {}, std::make_shared<PacketMemoryBudget>(8192), {}, limits);
  REQUIRE(recording.start(L"limits.mp4", Container::Mp4).is_success());
  REQUIRE(recording.accept(packet(0)).is_success());
  const bool entered = muxer.wait_entered();
  const std::vector batch{packet(1), packet(2)};
  const auto admitted = recording.accept_batch(batch);
  const auto before = recording.queue_metrics();
  const std::vector overflow{packet(3), packet(4)};
  const auto rejected = recording.accept_batch(overflow);
  const auto accepted = recording.packets_accepted();
  muxer.release(); const auto stopped = recording.stop();
  REQUIRE(entered); REQUIRE(admitted.is_success());
  CHECK(before.in_flight_bytes == 4); CHECK(before.queued_bytes == 8); CHECK(before.warning);
  CHECK_FALSE(rejected.is_success()); CHECK(accepted == 3);
  CHECK_FALSE(stopped.is_success()); CHECK(recording.queue_metrics().in_flight_bytes == 0);
}

TEST_CASE("continuous hard age uses enqueue time and retains the in flight budget until return", "[continuous-limits]") {
  FakeContinuousMuxer muxer; muxer.blocked = true;
  auto budget = std::make_shared<PacketMemoryBudget>(8192);
  ContinuousQueueLimits limits; limits.hard_age = std::chrono::milliseconds(20);
  ContinuousRecording recording(muxer, {}, budget, {}, limits);
  REQUIRE(recording.start(L"age.mp4", Container::Mp4).is_success());
  REQUIRE(recording.accept(packet(900000000)).is_success());
  const bool entered = muxer.wait_entered();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const auto health = recording.check_health();
  const auto charge = budget->bytes();
  muxer.release(); const auto stopped = recording.stop();
  REQUIRE(entered); REQUIRE_FALSE(health.is_success()); CHECK(health.error().code == "recording.queue_age_exceeded");
  CHECK(charge > 0); CHECK(budget->bytes() == 0); CHECK_FALSE(stopped.is_success());
}

TEST_CASE("continuous hard age is enforced after blocked writes including stop drain", "[continuous-limits]") {
  for (const bool stop_during_write : {false, true}) {
    CAPTURE(stop_during_write);
    FakeContinuousMuxer muxer; muxer.blocked = true;
    ContinuousQueueLimits limits; limits.hard_age = std::chrono::milliseconds(20);
    ContinuousRecording recording(muxer, {}, std::make_shared<PacketMemoryBudget>(8192), {}, limits);
    REQUIRE(recording.start(L"age-return.mp4", Container::Mp4).is_success());
    REQUIRE(recording.accept(packet(0)).is_success());
    const auto entered = muxer.wait_entered();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::optional<Result<void>> stopped;
    std::jthread stopper;
    bool stopped_accepting = true;
    if (stop_during_write) {
      stopper = std::jthread([&] { stopped.emplace(recording.stop()); });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (recording.active() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
      stopped_accepting = !recording.active();
    }
    muxer.release();
    if (stop_during_write) stopper.join(); else stopped.emplace(recording.stop());
    REQUIRE(entered); REQUIRE(stopped_accepting); REQUIRE(stopped.has_value());
    REQUIRE_FALSE(stopped->is_success());
    CHECK(stopped->error().code == "recording.queue_age_exceeded");
    CHECK(recording.failures() == 1);
  }
}


namespace {
class OrderedGateMuxer final : public IContinuousMuxer {
 public:
  Result<void> open(const std::vector<StreamDescriptor>&, const std::filesystem::path&, Container) override {
    return Result<void>::success();
  }
  Result<void> write(const EncodedPacket& packet) override {
    std::unique_lock lock(mutex);
    entered = true; condition.notify_all();
    condition.wait(lock, [&] { return released; });
    observed.emplace_back(packet.stream, packet.dts);
    if (fail_on_call != 0 && observed.size() == fail_on_call) {
      return Result<void>::failure({"mux.batch_injected", "Injected failure inside drained batch", {}});
    }
    return Result<void>::success();
  }
  Result<std::filesystem::path> finalize() override { return Result<std::filesystem::path>::success({}); }
  void abort() noexcept override {}
  bool wait_entered() {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
  }
  void release() { std::scoped_lock lock(mutex); released = true; condition.notify_all(); }
  std::mutex mutex;
  std::condition_variable condition;
  bool entered{false}, released{false};
  std::size_t fail_on_call{0};
  std::vector<std::pair<StreamKind, QpcTicks>> observed;
};
}

TEST_CASE("continuous batches preserve each producer FIFO while concurrent stops drain all accepted packets", "[continuous-limits]") {
  OrderedGateMuxer muxer;
  ContinuousRecording recording(muxer, {});
  REQUIRE(recording.start(L"multi-producer.mp4", Container::Mp4).is_success());
  REQUIRE(recording.accept(packet(-1)).is_success());
  const bool entered = muxer.wait_entered();
  std::atomic<bool> accepted{true};
  std::vector<std::jthread> producers;
  const std::array kinds{StreamKind::Video, StreamKind::MixedAudio, StreamKind::SystemAudio, StreamKind::MicrophoneAudio};
  for (auto kind : kinds) producers.emplace_back([&, kind] {
    for (int batch_index = 0; batch_index < 8; ++batch_index) {
      std::vector<EncodedPacket> batch;
      for (int i = 0; i < 4; ++i) batch.push_back(packet(batch_index * 4 + i, kind));
      if (!recording.accept_batch(batch).is_success()) accepted = false;
    }
  });
  producers.clear();
  bool stopped_one = false, stopped_two = false;
  std::jthread stop_one([&] { stopped_one = recording.stop().is_success(); });
  std::jthread stop_two([&] { stopped_two = recording.stop().is_success(); });
  muxer.release(); stop_one.join(); stop_two.join();
  REQUIRE(entered); REQUIRE(accepted.load()); REQUIRE(stopped_one); REQUIRE(stopped_two);
  REQUIRE(muxer.observed.size() == 129);
  for (auto kind : kinds) {
    QpcTicks expected = 0;
    for (auto [stream, dts] : muxer.observed) if (stream == kind && dts >= 0) CHECK(dts == expected++);
    CHECK(expected == 32);
  }
  CHECK(recording.queue_metrics().queued_packets == 0);
  CHECK(recording.queue_metrics().in_flight_packets == 0);
}

TEST_CASE("continuous stop racing with producer batches drains exactly the accepted stream prefixes", "[continuous-limits]") {
  const std::array kinds{StreamKind::Video, StreamKind::MixedAudio, StreamKind::SystemAudio, StreamKind::MicrophoneAudio};
  for (int round = 0; round < 16; ++round) {
    CAPTURE(round);
    OrderedGateMuxer muxer;
    auto budget = std::make_shared<PacketMemoryBudget>(64 * 1024);
    ContinuousRecording recording(muxer, {}, budget);
    REQUIRE(recording.start(L"admission-stop-race.mp4", Container::Mp4).is_success());
    REQUIRE(recording.accept(packet(-1)).is_success());
    const bool entered = muxer.wait_entered();
    std::barrier begin(6); // Four producers, stop, and the test thread.
    std::array<int, 4> admitted{};
    std::atomic<bool> valid_errors{true};
    std::vector<std::jthread> producers;
    for (std::size_t index = 0; index < kinds.size(); ++index) {
      producers.emplace_back([&, index] {
        begin.arrive_and_wait();
        for (int batch_index = 0; batch_index < 16; ++batch_index) {
          std::vector<EncodedPacket> batch;
          for (int i = 0; i < 4; ++i) batch.push_back(packet(batch_index * 4 + i, kinds[index]));
          const auto accepted = recording.accept_batch(batch);
          if (accepted.is_success()) admitted[index] += 4;
          else if (accepted.error().code != "recording.not_active") valid_errors = false;
        }
      });
    }
    bool stopped = false;
    std::jthread stopper([&] { begin.arrive_and_wait(); stopped = recording.stop().is_success(); });
    begin.arrive_and_wait();
    muxer.release();
    producers.clear();
    stopper.join();
    REQUIRE(entered);
    REQUIRE(stopped);
    REQUIRE(valid_errors.load());
    std::size_t total = 1;
    for (std::size_t index = 0; index < kinds.size(); ++index) {
      QpcTicks expected = 0;
      for (const auto& [stream, dts] : muxer.observed) {
        if (stream == kinds[index] && dts >= 0) CHECK(dts == expected++);
      }
      CHECK(expected == admitted[index]);
      total += admitted[index];
    }
    CHECK(muxer.observed.size() == total);
    CHECK(recording.packets_accepted() == total);
    CHECK(recording.packets_written() == total);
    CHECK(recording.failures() == 0);
    CHECK(recording.queue_metrics().queued_packets == 0);
    CHECK(recording.queue_metrics().in_flight_packets == 0);
    CHECK(recording.queue_metrics().oldest_enqueue_qpc == 0);
    CHECK(budget->bytes() == 0);
    CHECK(recording.accept(packet(100)).error().code == "recording.not_active");
  }
}

TEST_CASE("continuous writer wakes for successive isolated batches without a control command", "[continuous-limits]") {
  OrderedGateMuxer muxer;
  muxer.release();
  auto budget = std::make_shared<PacketMemoryBudget>(8192);
  ContinuousRecording recording(muxer, {}, budget);
  REQUIRE(recording.start(L"successive-wakes.mp4", Container::Mp4).is_success());
  for (int i = 0; i < 128; ++i) {
    REQUIRE(recording.accept(packet(i)).is_success());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (recording.packets_written() != static_cast<std::uint64_t>(i + 1) &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    // stop() itself sends a notification: assert progress before invoking it.
    REQUIRE(recording.packets_written() == static_cast<std::uint64_t>(i + 1));
  }
  REQUIRE(recording.stop().is_success());
  REQUIRE(muxer.observed.size() == 128);
  for (int i = 0; i < 128; ++i) CHECK(muxer.observed[i].second == i);
  CHECK(budget->bytes() == 0);
}

TEST_CASE("continuous drain batch is bounded but allows one oversized packet", "[continuous-limits]") {
  FakeContinuousMuxer muxer; muxer.blocked = true;
  ContinuousQueueLimits limits; limits.batch_packets = 2; limits.batch_bytes = 8;
  ContinuousRecording recording(muxer, {}, std::make_shared<PacketMemoryBudget>(8192), {}, limits);
  REQUIRE(recording.start(L"oversized-batch.mp4", Container::Mp4).is_success());
  auto oversized = packet(0); oversized.payload = std::make_shared<const std::vector<std::byte>>(16);
  const std::vector batch{oversized, packet(1), packet(2), packet(3)};
  const auto result = recording.accept_batch(batch);
  const bool entered = muxer.wait_entered(); const auto metrics = recording.queue_metrics();
  muxer.release(); const auto stopped = recording.stop();
  REQUIRE(result.is_success()); REQUIRE(entered); REQUIRE(stopped.is_success());
  CHECK(metrics.in_flight_bytes == 16); CHECK(metrics.in_flight_packets == 1);
  CHECK(metrics.queued_packets == 3); CHECK(recording.packets_written() == 4);
}

TEST_CASE("continuous failure inside a drained batch releases every charge and preserves the first error", "[continuous-limits]") {
  OrderedGateMuxer muxer;
  muxer.fail_on_call = 3;
  auto budget = std::make_shared<PacketMemoryBudget>(8192);
  ContinuousQueueLimits limits;
  limits.batch_packets = 4;
  ContinuousRecording recording(muxer, {}, budget, {}, limits);
  REQUIRE(recording.start(L"batch-failure.mp4", Container::Mp4).is_success());
  REQUIRE(recording.accept(packet(0)).is_success());
  const bool entered = muxer.wait_entered();
  bool admitted = false;
  std::weak_ptr<const std::vector<std::byte>> payload;
  {
    std::vector<EncodedPacket> batch;
    for (int i = 1; i <= 8; ++i) batch.push_back(packet(i));
    payload = batch.back().payload;
    admitted = recording.accept_batch(batch).is_success();
  }
  const auto retained = budget->bytes();
  muxer.release();
  const auto stopped = recording.stop();
  REQUIRE(entered);
  REQUIRE(admitted);
  REQUIRE(retained > 0);
  REQUIRE_FALSE(stopped.is_success());
  CHECK(stopped.error().code == "mux.batch_injected");
  CHECK(recording.stop().error().code == "mux.batch_injected");
  CHECK(recording.packets_accepted() == 9);
  CHECK(recording.packets_written() == 2);
  CHECK(muxer.observed.size() == 3);
  CHECK(recording.failures() == 1);
  CHECK(recording.queue_metrics().queued_packets == 0);
  CHECK(recording.queue_metrics().in_flight_packets == 0);
  CHECK(recording.queue_metrics().oldest_enqueue_qpc == 0);
  CHECK(budget->bytes() == 0);
  CHECK(payload.expired());

  muxer.fail_on_call = 0;
  REQUIRE(recording.start(L"after-batch-failure.mp4", Container::Mp4).is_success());
  REQUIRE(recording.accept(packet(10)).is_success());
  REQUIRE(recording.stop().is_success());
  CHECK(budget->bytes() == 0);
}

TEST_CASE("continuous soft warning clears with hysteresis and a new session resets queue state", "[continuous-limits]") {
  FakeContinuousMuxer muxer; muxer.blocked = true;
  ContinuousQueueLimits limits; limits.soft_bytes = 12; limits.hard_bytes = 24;
  ContinuousRecording recording(muxer, {}, std::make_shared<PacketMemoryBudget>(8192), {}, limits);
  REQUIRE(recording.start(L"hysteresis.mp4", Container::Mp4).is_success());
  const std::vector batch{packet(0), packet(1), packet(2), packet(3)};
  const auto result = recording.accept_batch(batch); const bool entered = muxer.wait_entered();
  const auto warning = recording.queue_metrics();
  muxer.release(); const auto stopped = recording.stop(); const auto drained = recording.queue_metrics();
  REQUIRE(result.is_success()); REQUIRE(entered); REQUIRE(stopped.is_success());
  CHECK(warning.warning); CHECK_FALSE(drained.warning); CHECK(drained.warning_transitions == 2);
  REQUIRE(recording.start(L"hysteresis-restart.mp4", Container::Mp4).is_success());
  CHECK(recording.queue_metrics().high_water_bytes == 0);
  REQUIRE(recording.accept(packet(4)).is_success()); REQUIRE(recording.stop().is_success());
}

TEST_CASE("FFmpeg continuous AVIO buffer is configurable and validates before touching destination") {
  const auto destination = std::filesystem::temp_directory_path() / "RebellioCap-invalid-buffer.mp4";
  FfmpegContinuousMuxer invalid(0);
  CHECK_FALSE(invalid.open(recovery_audio_descriptors(), destination, Container::Mp4).is_success());
  for (const std::size_t bytes : {std::size_t{32768}, std::size_t{256 * 1024}, std::size_t{1024 * 1024}}) {
    const auto path = std::filesystem::temp_directory_path() /
        ("RebellioCap-avio-" + std::to_string(bytes) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".mp4");
    FfmpegContinuousMuxer muxer(bytes);
    REQUIRE(muxer.open(recovery_audio_descriptors(), path, Container::Mp4).is_success());
    write_recovery_audio(muxer); REQUIRE(muxer.finalize().is_success());
    require_decodable_recovery(path); REQUIRE(std::filesystem::remove(path));
  }
}


TEST_CASE("FFmpeg interleaver retains shared packet budget until its copied buffer is released", "[memory-budget]") {
  const auto path = std::filesystem::temp_directory_path() /
      ("RebellioCap-interleave-budget-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".mp4");
  auto descriptors = recovery_audio_descriptors();
  descriptors.push_back({StreamKind::MicrophoneAudio, "aac", "Microphone", {std::byte{0x11}, std::byte{0x90}}, {1, 48000}, 0, 0, 48000, 2});
  auto budget = std::make_shared<PacketMemoryBudget>(8192);
  FfmpegContinuousMuxer muxer;
  REQUIRE(muxer.open(descriptors, path, Container::Mp4).is_success());
  {
    auto payload = budget->track(std::make_shared<const std::vector<std::byte>>(std::initializer_list<std::byte>{
        std::byte{0x21},std::byte{0x10},std::byte{0x04},std::byte{0x60},std::byte{0x8c},std::byte{0x1c}}));
    REQUIRE(payload.is_success());
    REQUIRE(muxer.write({StreamKind::SystemAudio, 1, 0, 0, 1024, true, std::move(payload).value()}).is_success());
  }
  // A second declared stream has not arrived: interleaver still owns the copy.
  CHECK(budget->bytes() > 0);
  REQUIRE(muxer.finalize().is_success());
  CHECK(budget->bytes() == 0);
  REQUIRE(std::filesystem::remove(path));
}
