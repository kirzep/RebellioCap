// Include the private pipeline implementation to inspect ownership on its COM
// thread; no test hooks or injectable devices are exposed in the shipping API.
#include "engine/hardware_pipeline_factory.cpp"
#include <fstream>

namespace rebelliocap {
namespace {
struct HardwareAudioPipelineTestAccess {
  static bool endpoint_recovery_encoding(QpcClock& clock, const std::string& fault = "audio.endpoint_lost") {
    struct Source final : IAudioCaptureSource {
      QpcTicks& now; QpcTicks lost_at; StreamKind stream; std::string fault; std::uint64_t position{0};
      Source(QpcTicks& time, QpcTicks loss, StreamKind kind, std::string error = "audio.endpoint_lost")
          : now(time), lost_at(loss), stream(kind), fault(std::move(error)) {}
      Result<std::optional<PcmBlock>> next_block(std::chrono::milliseconds) override {
        if (lost_at && now >= lost_at) return Result<std::optional<PcmBlock>>::failure({fault, "injected stream transition", {}});
        PcmBlock b{stream, now, 48'000, 2, std::vector<float>(480, .25F), 3};
        b.device_position_valid = true; b.device_position = position; position += 240;
        return Result<std::optional<PcmBlock>>::success(std::move(b));
      }
    };
    HardwareAudioPipeline pipeline(clock, L"pinned-render", L"pinned-mic");
    const auto start = clock.now(); auto now = start;
    int retries = 0;
    const auto install = [&](StreamKind stream, bool lose) {
      return std::make_unique<RecoveringAudioSource>(std::make_unique<Source>(now,
          lose ? start + clock.frequency() / 2 : 0, stream, fault), [&now, &clock, &retries, stream] {
        ++retries;
        return RecoveringAudioSource::SourceResult::success(std::make_unique<Source>(now, 0, stream));
      }, stream, clock.frequency(), [&now] { return now; }, [](auto) {});
    };
    pipeline.system_source_ = install(StreamKind::SystemAudio, false);
    pipeline.microphone_source_ = install(StreamKind::MicrophoneAudio, true);
    pipeline.system_normalizer_ = std::make_unique<PcmNormalizer>(clock);
    pipeline.microphone_normalizer_ = std::make_unique<PcmNormalizer>(clock);
    auto a = MfAacEncoder::create(StreamKind::SystemAudio, 160'000, clock);
    auto b = MfAacEncoder::create(StreamKind::MicrophoneAudio, 128'000, clock);
    auto c = MfAacEncoder::create(StreamKind::MixedAudio, 192'000, clock);
    if (!a.is_success() || !b.is_success() || !c.is_success()) return false;
    pipeline.system_encoder_ = std::move(a).value();
    pipeline.microphone_encoder_ = std::move(b).value();
    pipeline.mixed_encoder_ = std::move(c).value(); pipeline.initialized_ = true;
    std::vector<EncodedPacket> packets;
    bool saw_loss = false, saw_restored = false;
    for (int i = 0; i < 600; ++i) {
      now = start + static_cast<QpcTicks>(static_cast<long double>(i) * clock.frequency() / 200);
      auto result = pipeline.next(std::chrono::milliseconds(10));
      if (!result.is_success()) { std::cerr << "recovery next: " << result.error().message << '\n'; return false; }
      saw_loss |= pipeline.unavailable_audio_sources() == 2;
      if (saw_loss && pipeline.unavailable_audio_sources() == 0) saw_restored = true;
      packets.insert(packets.end(), result.value().begin(), result.value().end());
    }
    auto tail = pipeline.flush();
    if (!tail.is_success()) return false;
    packets.insert(packets.end(), tail.value().begin(), tail.value().end());
    if (!saw_loss || !saw_restored || retries != 1 || pipeline.unavailable_audio_sources() ||
        pipeline.mixing_dropped_frames()) return false;
    for (auto stream : {StreamKind::SystemAudio, StreamKind::MicrophoneAudio, StreamKind::MixedAudio}) {
      std::optional<QpcTicks> end; std::size_t count = 0;
      for (const auto& packet : packets) if (packet.stream == stream) {
        if (end && std::abs(packet.pts - *end) > (clock.frequency() + 47'999) / 48'000) return false;
        end = packet.pts + packet.duration; ++count;
      }
      if (count < 138 || !end || *end < start + clock.frequency() * 299 / 100) return false;
    }
    return true;
  }
  static bool overflow_metrics(QpcClock& clock) {
    HardwareAudioPipeline pipeline(clock, L"", L"", false, false);
    for (int cycle = 0; cycle < 2; ++cycle) {
      auto mixed = MfAacEncoder::create(StreamKind::MixedAudio, 192'000, clock);
      auto source = MfAacEncoder::create(StreamKind::SystemAudio, 160'000, clock);
      if (!mixed.is_success() || !source.is_success()) return false;
      pipeline.mixed_encoder_ = std::move(mixed).value();
      pipeline.system_encoder_ = std::move(source).value();
      pipeline.initialized_ = true;
      PcmBlock block{StreamKind::SystemAudio, clock.now(), 48'000, 2,
          std::vector<float>(96'000, .25F), 3};
      std::vector<EncodedPacket> output;
      if (!pipeline.accept_blocks({std::move(block)}, *pipeline.system_encoder_,
          StreamKind::SystemAudio, output).is_success()) return false;
      const auto expected = static_cast<std::uint64_t>((cycle + 1) * 24'000);
      if (pipeline.mixing_dropped_frames() != expected) return false;
      auto tail = pipeline.flush();
      if (!tail.is_success() || pipeline.mixing_dropped_frames() != expected) return false;
      output.insert(output.end(), tail.value().begin(), tail.value().end());
      std::optional<QpcTicks> first, end;
      for (const auto& packet : output) if (packet.stream == StreamKind::SystemAudio) {
        if (!first) first = packet.pts;
        end = packet.pts + packet.duration;
      }
      if (!first || !end || *end - *first < clock.frequency()) return false;
    }
    return true;
  }
  static bool normalized_clock_encoding(QpcClock& clock) {
    HardwareAudioPipeline pipeline(clock, L"", L"", false, false);
    auto mixed = MfAacEncoder::create(StreamKind::MixedAudio, 192'000, clock);
    auto system = MfAacEncoder::create(StreamKind::SystemAudio, 160'000, clock);
    auto microphone = MfAacEncoder::create(StreamKind::MicrophoneAudio, 128'000, clock);
    if (!mixed.is_success() || !system.is_success() || !microphone.is_success()) return false;
    pipeline.mixed_encoder_ = std::move(mixed).value();
    pipeline.system_encoder_ = std::move(system).value();
    pipeline.microphone_encoder_ = std::move(microphone).value();
    pipeline.initialized_ = true;
    PcmNormalizer system_normalizer(clock), microphone_normalizer(clock);
    std::vector<EncodedPacket> output;
    const auto accepted = [&](auto blocks, MfAacEncoder& encoder, StreamKind stream) {
      auto result = pipeline.accept_blocks(std::move(blocks), encoder, stream, output);
      if (!result.is_success()) std::cerr << "clock stream=" << static_cast<int>(stream) << ": " << result.error().message << '\n';
      return result.is_success();
    };
    const auto start = clock.now();
    for (int i = 0; i < 1000; ++i) {
      PcmBlock a{StreamKind::SystemAudio,
          start + static_cast<QpcTicks>(static_cast<long double>(i) * clock.frequency() / 100),
          48'000, 2, std::vector<float>(960, .1F), 3};
      // Exercise a buffered 44.1 kHz mono peer drifting 100 ppm. Source AAC
      // remains continuous while mixing consumes the independent capture clock.
      PcmBlock b{StreamKind::MicrophoneAudio,
          a.pts + static_cast<QpcTicks>(static_cast<long double>(i) * clock.frequency() / 1000000),
          44'100, 1, std::vector<float>(441, .2F), 4};
      a.device_position_valid = b.device_position_valid = true;
      a.device_position = static_cast<std::uint64_t>(i) * 480;
      b.device_position = static_cast<std::uint64_t>(i) * 441;
      auto ar = system_normalizer.normalize(std::move(a), StreamKind::SystemAudio);
      auto br = microphone_normalizer.normalize(std::move(b), StreamKind::MicrophoneAudio);
      if (!ar.is_success() || !br.is_success()) {
        std::cerr << "normalization at block " << i << '\n'; return false;
      }
      if (!accepted(std::move(ar).value(), *pipeline.system_encoder_, StreamKind::SystemAudio) ||
          !accepted(std::move(br).value(), *pipeline.microphone_encoder_, StreamKind::MicrophoneAudio)) return false;
    }
    auto ar = system_normalizer.flush(), br = microphone_normalizer.flush();
    if (!ar.is_success() || !br.is_success()) return false;
    if (!accepted(std::move(ar).value(), *pipeline.system_encoder_, StreamKind::SystemAudio) ||
        !accepted(std::move(br).value(), *pipeline.microphone_encoder_, StreamKind::MicrophoneAudio)) return false;
    auto tail = pipeline.flush();
    if (!tail.is_success()) { std::cerr << "clock flush: " << tail.error().message << '\n'; return false; }
    output.insert(output.end(), tail.value().begin(), tail.value().end());
    for (const auto stream : {StreamKind::MixedAudio, StreamKind::SystemAudio, StreamKind::MicrophoneAudio}) {
      std::optional<QpcTicks> end;
      std::optional<QpcTicks> previous;
      std::size_t count = 0;
      for (const auto& packet : output) if (packet.stream == stream) {
        // MFT packet duration and next sample time round independently with
        // irregular input chunks. Bound their mismatch to one PCM sample and
        // independently require increasing PTS at the 1024-frame AAC cadence.
        if (end && std::abs(packet.pts - *end) > (clock.frequency() + 47999) / 48000) {
          std::cerr << "clock packet boundary mismatch\n"; return false;
        }
        if (previous && (packet.pts <= *previous ||
            std::abs(packet.pts - *previous - clock.frequency() * 1024 / 48000) >
              (clock.frequency() + 47999) / 48000)) {
          std::cerr << "clock AAC cadence\n"; return false;
        }
        if (!count && packet.pts != start) { std::cerr << "clock first PTS\n"; return false; }
        end = packet.pts + packet.duration;
        previous = packet.pts;
        ++count;
      }
      if (count < 460 || !end || *end < start + clock.frequency() * 10) {
        std::cerr << "clock tail stream=" << static_cast<int>(stream) << " count=" << count
                  << " ticks=" << (end ? *end - start : 0) << '\n'; return false;
      }
    }
    return true;
  }
  static bool aligned_mixing_encoding(QpcClock& clock) {
    HardwareAudioPipeline pipeline(clock, L"", L"", false, false);
    auto encoder = MfAacEncoder::create(StreamKind::MixedAudio, 192'000, clock);
    if (!encoder.is_success()) return false;
    pipeline.mixed_encoder_ = std::move(encoder).value();
    pipeline.initialized_ = true;
    const auto start = clock.now();
    std::vector<EncodedPacket> output;
    for (int i = 0; i < 100; ++i) {
      PcmBlock system{StreamKind::SystemAudio,
          start + static_cast<QpcTicks>(static_cast<long double>(i) * clock.frequency() / 100),
          48'000, 2, std::vector<float>(960, .25F), 3};
      auto microphone = system;
      microphone.stream = StreamKind::MicrophoneAudio;
      microphone.pts += clock.frequency() / 200; // Starts 5 ms later.
      pipeline.windows_.push_system(system);
      // Synthetic gap/overlap in the mixing copy; source epoch recovery is E06.
      if (i != 20) pipeline.windows_.push_microphone(microphone);
      if (!pipeline.drain_mix(output).is_success()) return false;
    }
    auto tail = pipeline.flush();
    if (!tail.is_success()) return false;
    output.insert(output.end(), tail.value().begin(), tail.value().end());
    if (output.empty() || output.front().pts != start) return false;
    for (std::size_t i = 1; i < output.size(); ++i)
      if (output[i].pts <= output[i - 1].pts) return false;
    return output.back().pts + output.back().duration >= start + clock.frequency();
  }
  static bool missing_peer_encoding(QpcClock& clock) {
    HardwareAudioPipeline pipeline(clock, L"", L"", false, false);
    auto mixed = MfAacEncoder::create(StreamKind::MixedAudio, 192'000, clock);
    auto microphone = MfAacEncoder::create(StreamKind::MicrophoneAudio, 128'000, clock);
    if (!mixed.is_success() || !microphone.is_success()) return false;
    pipeline.mixed_encoder_ = std::move(mixed).value();
    pipeline.microphone_encoder_ = std::move(microphone).value();
    pipeline.initialized_ = true;
    std::size_t mixed_packets = 0, microphone_packets = 0;
    const auto start = clock.now();
    std::optional<QpcTicks> mixed_end, microphone_end;
    const auto inspect = [&](const auto& packets) {
      for (const auto& packet : packets) {
        auto& count = packet.stream == StreamKind::MixedAudio ? mixed_packets : microphone_packets;
        auto& end = packet.stream == StreamKind::MixedAudio ? mixed_end : microphone_end;
        if (end && packet.pts < *end - 1) return false;
        end = packet.pts + packet.duration;
        ++count;
      }
      return true;
    };
    // Five minutes of media time, not a five-minute physical WASAPI stall.
    for (int i = 0; i < 30'000; ++i) {
      PcmBlock input{StreamKind::MicrophoneAudio,
          start + static_cast<QpcTicks>(static_cast<long double>(i) * clock.frequency() / 100),
          48'000, 2, std::vector<float>(960, .25F), 3};
      std::vector<EncodedPacket> packets;
      auto accepted = pipeline.accept_blocks({std::move(input)}, *pipeline.microphone_encoder_,
                                            StreamKind::MicrophoneAudio, packets);
      if (!accepted.is_success() || !inspect(packets) ||
          pipeline.windows_.buffered_samples() > 96'000 || pipeline.windows_.dropped_frames()) return false;
      if (i == 99 && (!mixed_packets || !microphone_packets)) return false;
    }
    auto tail = pipeline.flush();
    if (!tail.is_success() || !inspect(tail.value()) || !mixed_end || !microphone_end) return false;
    const auto expected_end = start + clock.frequency() * 300;
    return *mixed_end >= expected_end && *microphone_end >= expected_end && empty(pipeline);
  }
  static void seed_pending(HardwareAudioPipeline& pipeline) {
    PcmBlock block{StreamKind::SystemAudio, 1, 48'000, 2, std::vector<float>(960, 0.5F)};
    pipeline.windows_.push_system(block);
    pipeline.windows_.push_microphone(block);
    pipeline.mixed_pts_ = 1;
  }
  static bool timeline_empty(HardwareAudioPipeline& pipeline) {
    return !pipeline.mixed_pts_ && !pipeline.windows_.pop();
  }
  static bool empty(const HardwareAudioPipeline& pipeline) {
    return !pipeline.system_source_ && !pipeline.microphone_source_ &&
           !pipeline.system_encoder_ && !pipeline.microphone_encoder_ &&
           !pipeline.mixed_encoder_ && !pipeline.system_normalizer_ &&
           !pipeline.microphone_normalizer_;
  }
};
struct HardwareVideoPipelineTestAccess {
  static bool resolution_rebuild(HardwareVideoPipeline& pipeline, QpcClock& clock) {
    auto& session = *pipeline.session_;
    const auto original_width = session.input_width_, original_height = session.input_height_;
    auto* encoder = session.encoder_.get();
    auto* cache = session.cache_.Get();
    if (session.rebuild_input(0, 600).is_success() || session.cache_.Get() != cache || session.input_width_ != original_width) return false;
    std::vector<EncodedPacket> packets;
    struct Mode { std::uint32_t width, height; DXGI_MODE_ROTATION rotation; };
    for (const auto mode : {Mode{800U,600U,DXGI_MODE_ROTATION_IDENTITY},
        Mode{1920U,1080U,DXGI_MODE_ROTATION_IDENTITY},
        Mode{original_width,original_height,DXGI_MODE_ROTATION_ROTATE90},
        Mode{original_width,original_height,DXGI_MODE_ROTATION_ROTATE180},
        Mode{original_width,original_height,DXGI_MODE_ROTATION_ROTATE270},
        Mode{original_width,original_height,DXGI_MODE_ROTATION_IDENTITY}}) {
      auto rebuilt = session.rebuild_input(mode.width, mode.height, mode.rotation);
      if (!rebuilt.is_success() || session.encoder_.get() != encoder || session.have_frame_ || !session.pending_keyframe_) return false;
      D3D11_TEXTURE2D_DESC description{};
      session.cache_->GetDesc(&description);
      if (description.Width != mode.width || description.Height != mode.height ||
          session.converter_->config().rotation != mode.rotation) return false;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
      if (FAILED(session.device_.device()->CreateRenderTargetView(session.cache_.Get(), nullptr, &target))) return false;
      const float color[]{.2F,.4F,.6F,1.F};
      session.device_.context()->ClearRenderTargetView(target.Get(), color);
      for (int frame = 0; frame < 12; ++frame) {
        const auto pts = clock.now();
        CapturedVideoFrame input{session.cache_, pts, mode.width, mode.height, session.cache_lifetime_, mode.rotation};
        auto converted = session.converter_->convert(input);
        if (!converted.is_success()) return false;
        auto encoded = session.encoder_->encode(std::move(converted).value(), pts, frame == 0);
        if (!encoded.is_success()) return false;
        for (auto& packet : encoded.value()) packets.push_back(std::move(packet));
        std::this_thread::sleep_for(std::chrono::milliseconds(17));
      }
    }
    auto tail = session.encoder_->flush();
    if (!tail.is_success()) return false;
    for (auto& packet : tail.value()) packets.push_back(std::move(packet));
    const auto idrs = std::count_if(packets.begin(), packets.end(), [](const auto& packet) { return packet.keyframe; });
    if (idrs != 6 || session.descriptor().width != 1280 || session.descriptor().height != 720) return false;
    const auto resources = session.encoder_->lifetime_metrics()->snapshot();
    // Rebuilding input never grows the NVENC registration cache with new pools.
    return resources.registrations_created <= 8;
  }
  static std::shared_ptr<const NvencLifetimeMetrics> lifetime(const HardwareVideoPipeline& pipeline) {
    return pipeline.session_->encoder_->lifetime_metrics();
  }
  static bool empty(const HardwareVideoPipeline& pipeline) { return !pipeline.session_; }
};
}
}

namespace {
int physical_mode_change(const char* output_path) {
  using namespace rebelliocap;
  auto monitors = enumerate_monitors();
  if (!monitors.is_success()) return 1;
  auto selected = primary_monitor(monitors.value());
  if (!selected.is_success()) return 77;
  const auto monitor = selected.value();
  DEVMODEW original{};
  original.dmSize = sizeof(original);
  if (!EnumDisplaySettingsW(monitor.name.c_str(), ENUM_CURRENT_SETTINGS, &original)) return 1;
  DEVMODEW alternative{};
  bool found = false;
  for (DWORD index = 0; ; ++index) {
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(monitor.name.c_str(), index, &mode)) break;
    if (mode.dmBitsPerPel != original.dmBitsPerPel || mode.dmDisplayFrequency != original.dmDisplayFrequency ||
        mode.dmDisplayOrientation != original.dmDisplayOrientation || mode.dmPelsWidth < 800 || mode.dmPelsHeight < 600 ||
        (mode.dmPelsWidth == original.dmPelsWidth && mode.dmPelsHeight == original.dmPelsHeight)) continue;
    if (ChangeDisplaySettingsExW(monitor.name.c_str(), &mode, nullptr, CDS_TEST, nullptr) == DISP_CHANGE_SUCCESSFUL) {
      alternative = mode; found = true;
      if (mode.dmPelsWidth == 1280 && mode.dmPelsHeight == 720) break;
    }
  }
  if (!found) { std::cerr << "No alternate supported resolution at current refresh rate\n"; return 77; }
  QpcClock clock;
  if (!HardwareAudioPipelineTestAccess::endpoint_recovery_encoding(clock)) {
    std::cerr << "endpoint recovery failed continuous real AAC tracks or degraded metrics\n";
    return 1;
  }
  CliArguments arguments; arguments.width = 1280; arguments.height = 720;
  auto created = HardwareVideoPipeline::create(monitor, arguments, clock);
  if (!created.is_success()) return 1;
  auto video = std::move(created).value();
  std::ofstream output(output_path, std::ios::binary);
  if (!output) return 1;
  std::size_t count = 0, idrs = 0;
  QpcTicks last_dts = (std::numeric_limits<QpcTicks>::min)();
  auto accept = [&](const std::vector<EncodedPacket>& packets) {
    for (const auto& packet : packets) {
      if (packet.dts <= last_dts || !packet.payload) return false;
      last_dts = packet.dts; ++count; if (packet.keyframe) ++idrs;
      output.write(reinterpret_cast<const char*>(packet.payload->data()), static_cast<std::streamsize>(packet.payload->size()));
      if (!output) return false;
    }
    return true;
  };
  auto capture = [&]() {
    const auto initial = count;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (count < initial + 90 && std::chrono::steady_clock::now() < deadline) {
      auto packets = video->tick(clock.now(), count == 0);
      if (!packets.is_success()) { std::cerr << packets.error().code << ": " << packets.error().message << '\n'; return false; }
      if (!accept(packets.value())) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(17));
    }
    return count >= initial + 90;
  };
  if (!capture()) return 1;
  struct Restore {
    std::wstring name; DEVMODEW mode; bool active{true};
    bool restore() { if (!active) return true; if (ChangeDisplaySettingsExW(name.c_str(), &mode, nullptr, 0, nullptr) != DISP_CHANGE_SUCCESSFUL) return false; active = false; return true; }
    ~Restore() { if (active) restore(); }
  } restore{monitor.name, original};
  std::cout << "Display " << original.dmPelsWidth << 'x' << original.dmPelsHeight << " -> " << alternative.dmPelsWidth << 'x' << alternative.dmPelsHeight << '\n';
  if (ChangeDisplaySettingsExW(monitor.name.c_str(), &alternative, nullptr, 0, nullptr) != DISP_CHANGE_SUCCESSFUL || !capture()) return 1;
  if (!restore.restore() || !capture()) return 1;
  auto tail = video->flush();
  if (!tail.is_success() || !accept(tail.value()) || idrs < 3) return 1;
  const auto resources = HardwareVideoPipelineTestAccess::lifetime(*video)->snapshot();
  if (resources.registrations_created > 8) return 1;
  DEVMODEW actual{}; actual.dmSize = sizeof(actual);
  if (!EnumDisplaySettingsW(monitor.name.c_str(), ENUM_CURRENT_SETTINGS, &actual) || actual.dmPelsWidth != original.dmPelsWidth || actual.dmPelsHeight != original.dmPelsHeight) return 1;
  std::cout << "Physical mode changes restored display; packets=" << count << " IDRs=" << idrs << " registrations=" << resources.registrations_created << " output=1280x720\n";
  return 0;
}
}

int main(int argc, char** argv) {
  using namespace rebelliocap;
  ComApartment apartment;
  if (!apartment.initialized()) return 77;
  if (argc == 3 && std::string(argv[1]) == "--physical-mode-change") return physical_mode_change(argv[2]);
  auto endpoints = enumerate_system_audio();
  if (!endpoints.is_success() || endpoints.value().empty()) return 77;
  QpcClock clock;
  for (const auto& fault : {"audio.endpoint_lost", "audio.timestamp_unreliable"}) {
    if (!HardwareAudioPipelineTestAccess::endpoint_recovery_encoding(clock, fault)) {
      std::cerr << "stream recovery failed continuous real AAC tracks: " << fault << '\n';
      return 1;
    }
  }
  if (!HardwareAudioPipelineTestAccess::overflow_metrics(clock)) {
    std::cerr << "mixing overflow metrics lost counts or source AAC samples\n";
    return 1;
  }
  if (!HardwareAudioPipelineTestAccess::normalized_clock_encoding(clock)) {
    std::cerr << "capture clock normalization broke mixed or separate AAC streams\n";
    return 1;
  }
  if (!HardwareAudioPipelineTestAccess::aligned_mixing_encoding(clock)) {
    std::cerr << "QPC aligned mixed PCM failed real AAC encoding\n";
    return 1;
  }
  if (!HardwareAudioPipelineTestAccess::missing_peer_encoding(clock)) {
    std::cerr << "missing peer stopped AAC mixing or lost the microphone track\n";
    return 1;
  }
  // The system side opens successfully, then microphone startup fails. All
  // acquired COM objects must be released before the owning worker exits.
  HardwareAudioPipeline pipeline(clock, endpoints.value().front().id,
                                 L"{rebelliocap-deliberately-missing-endpoint}");
  auto started = pipeline.next(std::chrono::milliseconds(0));
  if (started.is_success()) return 1;
  HardwareAudioPipelineTestAccess::seed_pending(pipeline);
  auto flushed = pipeline.flush();
  if (!flushed.is_success() || !HardwareAudioPipelineTestAccess::empty(pipeline) ||
      !HardwareAudioPipelineTestAccess::timeline_empty(pipeline)) {
    std::cerr << "partial audio startup left COM resources on the worker\n";
    return 1;
  }
  HardwareAudioPipeline audio(clock, endpoints.value().front().id, L"", true, false);
  for (int cycle = 0; cycle < 5; ++cycle) {
    auto next = audio.next(std::chrono::milliseconds(20));
    if (!next.is_success()) { std::cerr << "audio resume: " << next.error().code << '\n'; return 1; }
    if (HardwareAudioPipelineTestAccess::empty(audio)) { std::cerr << "audio not initialized\n"; return 1; }
    auto stopped = audio.flush();
    if (!stopped.is_success()) { std::cerr << "audio drain: " << stopped.error().code << ": " << stopped.error().message << " HRESULT=" << stopped.error().hresult.value_or(0) << '\n'; return 1; }
    if (!HardwareAudioPipelineTestAccess::empty(audio) ||
        !HardwareAudioPipelineTestAccess::timeline_empty(audio)) { std::cerr << "audio state retained\n"; return 1; }
  }

  auto monitors = enumerate_monitors();
  if (!monitors.is_success()) return 1;
  auto monitor = primary_monitor(monitors.value());
  if (!monitor.is_success()) return 77;
  CliArguments arguments;
  arguments.width = 1280;
  arguments.height = 720;
  auto created = HardwareVideoPipeline::create(monitor.value(), arguments, clock);
  if (!created.is_success()) {
    std::cerr << created.error().code << ": " << created.error().message << '\n';
    return 1;
  }
  auto video = std::move(created).value();
  if (!HardwareVideoPipelineTestAccess::resolution_rebuild(*video, clock)) {
    std::cerr << "resolution rebuild lost encoder continuity, dimensions, IDR or bounded registrations\n";
    return 1;
  }
  video->suspend();
  for (int cycle = 0; cycle < 5; ++cycle) {
    auto resumed = video->resume();
    if (!resumed.is_success()) { std::cerr << "video resume: " << resumed.error().code << '\n'; return 1; }
    auto lifetime = HardwareVideoPipelineTestAccess::lifetime(*video);
    std::vector<EncodedPacket> packets;
    for (int frame = 0; frame < 12; ++frame) {
      auto encoded = video->tick(clock.now(), frame == 0);
      if (!encoded.is_success()) { std::cerr << "video tick: " << encoded.error().code << '\n'; return 1; }
      for (auto& packet : encoded.value()) packets.push_back(std::move(packet));
      std::this_thread::sleep_for(std::chrono::milliseconds(17));
    }
    auto tail = video->flush();
    if (!tail.is_success()) { std::cerr << "video drain: " << tail.error().code << '\n'; return 1; }
    for (auto& packet : tail.value()) packets.push_back(std::move(packet));
    if (packets.empty() || !packets.front().keyframe) { std::cerr << "video missing first IDR\n"; return 1; }
    video->suspend();
    const auto freed = lifetime->snapshot();
    if (!HardwareVideoPipelineTestAccess::empty(*video) ||
        freed.sessions_created != freed.sessions_released ||
        freed.registrations_created != freed.registrations_released ||
        freed.maps_created != freed.maps_released ||
        freed.events_created != freed.events_released ||
        freed.bitstreams_created != freed.bitstreams_released || freed.in_flight != 0) {
      std::cerr << "idle retained GPU resources\n";
      return 1;
    }
  }
  std::cout << "Five audio/video resume cycles released all resources and began with IDR\n";
  return 0;
}
