#include <Windows.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include "audio/audio_mixer.h"
#include "audio/audio_endpoint_catalog.h"
#include "audio/mf_aac_encoder.h"
#include "audio/pcm_normalizer.h"
#include "audio/pcm_windowizer.h"
#include "audio/pcm_epoch_bridge.h"
#include "audio/recovering_audio_source.h"
#include "audio/wasapi_capture_source.h"
#include "capture/d3d11_device.h"
#include "capture/dxgi_capture_source.h"
#include "capture/recovering_capture_source.h"
#include "engine/game_category.h"
#include "capture/monitor_catalog.h"
#include "cli/arguments.h"
#include "cli/engine_metrics_json.h"
#include "core/diagnostic_writer.h"
#include "engine/recorder_engine.h"
#include "engine/hardware_pipeline_factory.h"
#include <sstream>
#include "hotkey/raw_input_hotkey.h"
#include "mux/ffmpeg_clip_muxer.h"
#include "mux/ffmpeg_continuous_muxer.h"
#include "platform/windows/com_apartment.h"
#include "video/d3d11_nv12_converter.h"
#include "video/nvenc_api.h"
#include "video/nvenc_encoder.h"

namespace rebelliocap {
namespace {

using namespace std::chrono_literals;

std::atomic_bool stop_requested{false};

BOOL WINAPI console_control(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
    stop_requested = true;
    return TRUE;
  }
  return FALSE;
}

Error pipeline_error(std::string code, std::string message,
                     std::optional<long> native = std::nullopt) {
  return {.code = std::move(code), .message = std::move(message), .hresult = native};
}

std::string utf8(std::wstring_view value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return "[invalid-utf16]";
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}

CliExitCode exit_for(const Error& error) {
  if (error.code.starts_with("cli.")) return CliExitCode::InvalidArguments;
  if (error.code.starts_with("audio") || error.code.starts_with("mf_audio")) {
    return CliExitCode::AudioFailure;
  }
  if (error.code.starts_with("mux")) return CliExitCode::MuxFailure;
  if (error.code.starts_with("dxgi_capture")) return CliExitCode::CaptureFailure;
  return CliExitCode::UnsupportedHardware;
}

void write_error(DiagnosticWriter& writer, QpcClock& clock, const Error& error) {
  writer.write({"error", clock.now(), {{"code", error.code},
                                        {"message", error.message},
                                        {"hresult", error.hresult.value_or(0)}}});
  std::cerr << error.code << ": " << error.message << '\n';
}

class HardwareVideoSession final : public IEngineVideoPipeline {
  friend struct HardwareVideoPipelineTestAccess;
 public:
  static Result<std::shared_ptr<HardwareVideoSession>> create(
      const MonitorInfo& monitor, const CliArguments& arguments, QpcClock& clock) {
    auto device_result = create_d3d11_device(monitor.id.adapter_luid);
    if (!device_result.is_success()) return Result<std::shared_ptr<HardwareVideoSession>>::failure(device_result.error());
    auto device = std::move(device_result).value();
    auto capture_result = DxgiCaptureSource::create(device, monitor.id, clock);
    if (!capture_result.is_success()) return Result<std::shared_ptr<HardwareVideoSession>>::failure(capture_result.error());
    auto capture = std::move(capture_result).value();
    const auto mode = capture->description().ModeDesc;
    D3d11Nv12ConverterConfig converter_config{mode.Width, mode.Height, arguments.width,
                                               arguments.height, 8, capture->description().Rotation};
    auto converter_result = D3d11Nv12Converter::create(
        device.device(), device.context(), converter_config);
    if (!converter_result.is_success()) return Result<std::shared_ptr<HardwareVideoSession>>::failure(converter_result.error());
    NvencConfig encoder_config{arguments.width, arguments.height, arguments.fps,
                               arguments.bitrate, 2};
    auto encoder_result = NvencEncoder::create(device.device(), encoder_config, clock);
    if (!encoder_result.is_success()) return Result<std::shared_ptr<HardwareVideoSession>>::failure(encoder_result.error());

    D3D11_TEXTURE2D_DESC cache_description{};
    cache_description.Width = mode.Width;
    cache_description.Height = mode.Height;
    cache_description.MipLevels = 1;
    cache_description.ArraySize = 1;
    cache_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    cache_description.SampleDesc.Count = 1;
    cache_description.Usage = D3D11_USAGE_DEFAULT;
    cache_description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cache;
    const HRESULT created = device.device()->CreateTexture2D(&cache_description, nullptr, &cache);
    if (FAILED(created)) {
      return Result<std::shared_ptr<HardwareVideoSession>>::failure(
          pipeline_error("capture.cache_creation_failed", "Could not create the GPU-only latest-frame cache.", static_cast<long>(created)));
    }
    auto result = std::shared_ptr<HardwareVideoSession>(new HardwareVideoSession(
        std::move(device), std::move(capture), std::move(converter_result).value(),
        std::move(encoder_result).value(), std::move(cache), mode.Width, mode.Height, monitor, clock));
    return Result<std::shared_ptr<HardwareVideoSession>>::success(std::move(result));
  }

  Result<std::vector<EncodedPacket>> tick(QpcTicks pts, bool force_keyframe) override {
    pending_keyframe_ = pending_keyframe_ || force_keyframe;
    auto acquired = capture_->next_frame(have_frame_ ? 0ms : 100ms);
    if (!acquired.is_success()) return Result<std::vector<EncodedPacket>>::failure(acquired.error());
    if (capture_->suspended()) {
      have_frame_ = false;
      pending_keyframe_ = true;
      return Result<std::vector<EncodedPacket>>::success({});
    }
    if (acquired.value().has_value()) {
      auto frame = std::move(acquired.value().value());
      // First-frame waits and recovery may finish after the scheduled tick.
      // Keep newly acquired content on the same current QPC clock as audio.
      pts = (std::max)(pts, frame.captured_at);
      if (frame.width != input_width_ || frame.height != input_height_ ||
          frame.rotation != converter_->config().rotation) {
        auto rebuilt = rebuild_input(frame.width, frame.height, frame.rotation);
        if (!rebuilt.is_success()) return Result<std::vector<EncodedPacket>>::failure(rebuilt.error());
      }
      device_.context()->CopyResource(cache_.Get(), frame.texture.Get());
      ++content_revision_;
      have_frame_ = true;
    }
    if (!have_frame_) return Result<std::vector<EncodedPacket>>::success({});
    CapturedVideoFrame cached{cache_, pts, input_width_, input_height_, cache_lifetime_};
    auto converted = converter_->convert_cached(cached, content_revision_);
    if (!converted.is_success()) return Result<std::vector<EncodedPacket>>::failure(converted.error());
    auto encoded = encoder_->encode(std::move(converted).value(), pts, pending_keyframe_);
    if (encoded.is_success()) pending_keyframe_ = false;
    return encoded;
  }

  Result<std::vector<EncodedPacket>> flush() override { return encoder_->flush(); }
  const StreamDescriptor& descriptor() const noexcept { return encoder_->descriptor(); }
  NvencRuntimeMetrics metrics() const { return encoder_->metrics(); }

 private:
  Result<void> rebuild_input(std::uint32_t width, std::uint32_t height,
      DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY) {
    D3D11_TEXTURE2D_DESC description{};
    cache_->GetDesc(&description);
    description.Width = width;
    description.Height = height;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> replacement;
    const HRESULT created = device_.device()->CreateTexture2D(&description, nullptr, &replacement);
    if (FAILED(created)) return Result<void>::failure(pipeline_error(
        "capture.cache_creation_failed", "Could not rebuild the latest-frame cache.", static_cast<long>(created)));
    auto rebuilt = converter_->reconfigure_input(width, height, rotation);
    if (!rebuilt.is_success()) return rebuilt;
    // Output resolution, NVENC, registered output surfaces and container
    // descriptors remain stable; only the capture-sized input changes.
    cache_ = std::move(replacement);
    input_width_ = width;
    input_height_ = height;
    have_frame_ = false;
    content_revision_ = 0;
    pending_keyframe_ = true;
    return Result<void>::success();
  }

  HardwareVideoSession(D3d11Device device, std::unique_ptr<DxgiCaptureSource> capture,
                        std::unique_ptr<D3d11Nv12Converter> converter,
                        std::unique_ptr<NvencEncoder> encoder,
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> cache,
                        std::uint32_t width, std::uint32_t height,
                        const MonitorInfo& monitor, QpcClock& clock)
      : device_(std::move(device)),
        converter_(std::move(converter)), encoder_(std::move(encoder)),
        cache_(std::move(cache)), input_width_(width), input_height_(height),
        cache_lifetime_(std::make_shared<int>(0)) {
    capture_ = std::make_unique<RecoveringCaptureSource>(std::move(capture),
      [this, monitor, &clock]() -> RecoveringCaptureSource::SourceResult {
        const HRESULT removed = device_.device()->GetDeviceRemovedReason();
        if (FAILED(removed)) return RecoveringCaptureSource::SourceResult::failure(
          pipeline_error("capture.device_removed", "The graphics device was removed.", static_cast<long>(removed)));
        auto monitors = enumerate_monitors();
        if (!monitors.is_success()) return RecoveringCaptureSource::SourceResult::failure(monitors.error());
        const auto found = select_monitor(monitor.id, monitors.value());
        if (!found.is_success()) return RecoveringCaptureSource::SourceResult::failure(
          pipeline_error("capture.monitor_unavailable", "Waiting for the selected monitor to return."));
        auto recreated = DxgiCaptureSource::create(device_, found.value().id, clock);
        if (!recreated.is_success()) return RecoveringCaptureSource::SourceResult::failure(recreated.error());
        const auto mode = recreated.value()->description().ModeDesc;
        const auto rotation = recreated.value()->description().Rotation;
        if (mode.Width != input_width_ || mode.Height != input_height_ ||
            rotation != converter_->config().rotation) {
          auto rebuilt = rebuild_input(mode.Width, mode.Height, rotation);
          if (!rebuilt.is_success()) return RecoveringCaptureSource::SourceResult::failure(rebuilt.error());
        }
        return RecoveringCaptureSource::SourceResult::success(std::move(recreated).value());
      }, [](const std::string& event, const Error& error) {
        std::cerr << event << ": " << error.code << ": " << error.message
                  << " (HRESULT=" << error.hresult.value_or(0) << ")\n";
      });
  }

  D3d11Device device_;
  std::unique_ptr<RecoveringCaptureSource> capture_;
  std::unique_ptr<D3d11Nv12Converter> converter_;
  std::unique_ptr<NvencEncoder> encoder_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> cache_;
  std::uint32_t input_width_;
  std::uint32_t input_height_;
  std::shared_ptr<const void> cache_lifetime_;
  bool have_frame_{false};
  std::uint64_t content_revision_{0};
  bool pending_keyframe_{false};
};

// Keep descriptors alive for pending saves while releasing the entire GPU
// session on the capture worker during idle. A flushed NVENC session cannot
// be reused; resume creates a fresh one with the same stream configuration.
class HardwareVideoPipeline final : public IEngineVideoPipeline {
  friend struct HardwareVideoPipelineTestAccess;
 public:
  static Result<std::shared_ptr<HardwareVideoPipeline>> create(
      const MonitorInfo& monitor, const CliArguments& arguments, QpcClock& clock) {
    auto session = HardwareVideoSession::create(monitor, arguments, clock);
    if (!session.is_success()) return Result<std::shared_ptr<HardwareVideoPipeline>>::failure(session.error());
    return Result<std::shared_ptr<HardwareVideoPipeline>>::success(
        std::shared_ptr<HardwareVideoPipeline>(new HardwareVideoPipeline(
            monitor, arguments, clock, std::move(session).value())));
  }

  Result<void> resume() override {
    std::scoped_lock lock(mutex_);
    if (session_) return Result<void>::success();
    auto session = HardwareVideoSession::create(monitor_, arguments_, clock_);
    if (!session.is_success()) return Result<void>::failure(session.error());
    const auto& next = session.value()->descriptor();
    if (next.codec != descriptor_.codec || next.codec_extradata != descriptor_.codec_extradata ||
        next.width != descriptor_.width || next.height != descriptor_.height) {
      return Result<void>::failure(pipeline_error("capture.stream_changed",
          "Video stream configuration changed; restart the engine."));
    }
    session_ = std::move(session).value();
    return Result<void>::success();
  }

  void suspend() noexcept override {
    std::scoped_lock lock(mutex_);
    if (session_) {
      const auto current = session_->metrics();
      accumulated_.submitted_frames += current.submitted_frames;
      accumulated_.completed_frames += current.completed_frames;
      accumulated_.p_picture_frames += current.p_picture_frames;
      accumulated_.b_picture_frames += current.b_picture_frames;
      accumulated_.driver_timestamp_packets += current.driver_timestamp_packets;
      accumulated_.driver_timestamp_digest ^= current.driver_timestamp_digest;
      accumulated_.p50_latency_us = current.p50_latency_us;
      accumulated_.p95_latency_us = current.p95_latency_us;
      session_.reset();
    }
  }

  Result<std::vector<EncodedPacket>> tick(QpcTicks pts, bool force_keyframe) override {
    std::scoped_lock lock(mutex_);
    return session_->tick(pts, force_keyframe);
  }
  Result<std::vector<EncodedPacket>> flush() override {
    std::scoped_lock lock(mutex_);
    return session_ ? session_->flush() : Result<std::vector<EncodedPacket>>::success({});
  }
  const StreamDescriptor& descriptor() const noexcept { return descriptor_; }
  NvencRuntimeMetrics metrics() const {
    std::scoped_lock lock(mutex_);
    auto result = session_ ? session_->metrics() : NvencRuntimeMetrics{};
    result.submitted_frames += accumulated_.submitted_frames;
    result.completed_frames += accumulated_.completed_frames;
    result.p_picture_frames += accumulated_.p_picture_frames;
    result.b_picture_frames += accumulated_.b_picture_frames;
    result.driver_timestamp_packets += accumulated_.driver_timestamp_packets;
    result.driver_timestamp_digest ^= accumulated_.driver_timestamp_digest;
    if (!session_) {
      result.p50_latency_us = accumulated_.p50_latency_us;
      result.p95_latency_us = accumulated_.p95_latency_us;
    }
    return result;
  }

 private:
  HardwareVideoPipeline(const MonitorInfo& monitor, const CliArguments& arguments,
                        QpcClock& clock, std::shared_ptr<HardwareVideoSession> session)
      : monitor_(monitor), arguments_(arguments), clock_(clock),
        descriptor_(session->descriptor()), session_(std::move(session)) {}
  MonitorInfo monitor_;
  CliArguments arguments_;
  QpcClock& clock_;
  StreamDescriptor descriptor_;
  mutable std::mutex mutex_;
  std::shared_ptr<HardwareVideoSession> session_;
  NvencRuntimeMetrics accumulated_;
};

class HardwareAudioPipeline final : public IEngineAudioPipeline {
  friend struct HardwareAudioPipelineTestAccess;
 public:
  std::uint32_t unavailable_audio_sources() const noexcept override {
    return unavailable_sources_.load(std::memory_order_relaxed);
  }
  std::uint64_t mixing_dropped_frames() const noexcept override {
    return mixing_dropped_frames_.load(std::memory_order_relaxed);
  }
  HardwareAudioPipeline(QpcClock& clock, std::wstring system_audio_id,
                        std::wstring microphone_id, bool system_enabled = true, bool microphone_enabled = true)
      : clock_(clock), windows_(clock.frequency()), system_bridge_(clock.frequency()), microphone_bridge_(clock.frequency()),
        system_audio_id_(std::move(system_audio_id)),
        microphone_id_(std::move(microphone_id)),
        system_enabled_(system_enabled), microphone_enabled_(microphone_enabled) {}

  Result<std::vector<EncodedPacket>> next(std::chrono::milliseconds timeout) override {
    if (!initialized_) {
      auto initialized = initialize();
      // A later endpoint/encoder may fail after the system source was opened.
      // Release partial COM state on this worker, never on engine destruction.
      if (!initialized.is_success()) return finish(Result<std::vector<EncodedPacket>>::failure(initialized.error()));
    }
    std::vector<EncodedPacket> output;
    if (system_enabled_) {
    auto system = pump(*system_source_, *system_normalizer_, *system_encoder_,
                       StreamKind::SystemAudio, timeout / 2, output);
    if (!system.is_success()) return Result<std::vector<EncodedPacket>>::failure(system.error());
    }
    if (microphone_enabled_) {
    auto microphone = pump(*microphone_source_, *microphone_normalizer_, *microphone_encoder_,
                           StreamKind::MicrophoneAudio, timeout / 2, output);
    if (!microphone.is_success()) return Result<std::vector<EncodedPacket>>::failure(microphone.error());
    }
    auto mixed = drain_mix(output);
    if (!mixed.is_success()) return Result<std::vector<EncodedPacket>>::failure(mixed.error());
    return Result<std::vector<EncodedPacket>>::success(std::move(output));
  }

  Result<std::vector<EncodedPacket>> flush() override {
    std::vector<EncodedPacket> output;
    if (!initialized_) return finish(Result<std::vector<EncodedPacket>>::success({}));
    if (system_enabled_) {
    auto system = flush_normalizer(*system_normalizer_, *system_encoder_, StreamKind::SystemAudio,
                                   output);
    if (!system.is_success()) return finish(Result<std::vector<EncodedPacket>>::failure(system.error()));
    }
    if (microphone_enabled_) {
    auto microphone = flush_normalizer(*microphone_normalizer_, *microphone_encoder_,
                                       StreamKind::MicrophoneAudio, output);
    if (!microphone.is_success()) return finish(Result<std::vector<EncodedPacket>>::failure(microphone.error()));
    }
    auto mixed = drain_mix(output, true);
    if (!mixed.is_success()) return finish(Result<std::vector<EncodedPacket>>::failure(mixed.error()));
    for (auto* encoder : {mixed_encoder_.get(), system_encoder_.get(), microphone_encoder_.get()}) {
      if (!encoder) continue;
      auto tail = encoder->flush();
      if (!tail.is_success()) return finish(Result<std::vector<EncodedPacket>>::failure(tail.error()));
      output.insert(output.end(), std::make_move_iterator(tail.value().begin()),
                    std::make_move_iterator(tail.value().end()));
    }
    return finish(Result<std::vector<EncodedPacket>>::success(std::move(output)));
  }

 private:
  Result<void> initialize() {
    if (system_enabled_) {
      auto source = WasapiCaptureSource::create({AudioEndpointRole::SystemLoopback, system_audio_id_}, clock_);
      if (!source.is_success()) return Result<void>::failure(source.error());
      auto encoder = MfAacEncoder::create(StreamKind::SystemAudio, 160'000, clock_);
      if (!encoder.is_success()) return Result<void>::failure(encoder.error());
      system_source_ = recovering_source(std::move(source).value(), AudioEndpointRole::SystemLoopback, system_audio_id_, StreamKind::SystemAudio);
      system_encoder_ = std::move(encoder).value();
      system_normalizer_ = std::make_unique<PcmNormalizer>(clock_);
    }
    if (microphone_enabled_) {
      auto source = WasapiCaptureSource::create({AudioEndpointRole::Microphone, microphone_id_}, clock_);
      if (!source.is_success()) return Result<void>::failure(source.error());
      auto encoder = MfAacEncoder::create(StreamKind::MicrophoneAudio, 128'000, clock_);
      if (!encoder.is_success()) return Result<void>::failure(encoder.error());
      microphone_source_ = recovering_source(std::move(source).value(), AudioEndpointRole::Microphone, microphone_id_, StreamKind::MicrophoneAudio);
      microphone_encoder_ = std::move(encoder).value();
      microphone_normalizer_ = std::make_unique<PcmNormalizer>(clock_);
    }
    if (system_enabled_ && microphone_enabled_) {
      auto encoder = MfAacEncoder::create(StreamKind::MixedAudio, 192'000, clock_);
      if (!encoder.is_success()) return Result<void>::failure(encoder.error());
      mixed_encoder_ = std::move(encoder).value();
    }
    initialized_ = true;
    return Result<void>::success();
  }

 Result<void> append_encoded(MfAacEncoder& encoder, const PcmBlock& block,
                              std::vector<EncodedPacket>& output) {
    auto packets = encoder.encode(block);
    if (!packets.is_success()) return Result<void>::failure(packets.error());
    output.insert(output.end(), std::make_move_iterator(packets.value().begin()),
                  std::make_move_iterator(packets.value().end()));
    return Result<void>::success();
  }

  std::unique_ptr<RecoveringAudioSource> recovering_source(std::unique_ptr<WasapiCaptureSource> source,
      AudioEndpointRole role, std::wstring id, StreamKind stream) {
    return std::make_unique<RecoveringAudioSource>(std::move(source), [this, role, id = std::move(id)] {
      auto result = WasapiCaptureSource::create({role, id}, clock_);
      if (!result.is_success()) return RecoveringAudioSource::SourceResult::failure(result.error());
      return RecoveringAudioSource::SourceResult::success(std::move(result).value());
    }, stream, clock_.frequency(), [this] { return clock_.now(); },
    [](auto timeout) { std::this_thread::sleep_for(timeout); });
  }

  Result<std::vector<EncodedPacket>> finish(
      Result<std::vector<EncodedPacket>> result) {
    mixed_encoder_.reset();
    system_encoder_.reset();
    microphone_encoder_.reset();
    system_normalizer_.reset();
    microphone_normalizer_.reset();
    system_source_.reset();
    microphone_source_.reset();
    initialized_ = false;
    windows_ = SynchronizedPcmWindows(clock_.frequency());
    system_bridge_ = PcmEpochBridge(clock_.frequency());
    microphone_bridge_ = PcmEpochBridge(clock_.frequency());
    unavailable_sources_ = 0;
    mixed_pts_.reset();
    return result;
  }

  Result<void> accept_blocks(std::vector<PcmBlock> blocks, MfAacEncoder& encoder,
                             StreamKind stream, std::vector<EncodedPacket>& output) {
    for (auto& block : blocks) {
      auto& bridge = stream == StreamKind::SystemAudio ? system_bridge_ : microphone_bridge_;
      auto accepted = bridge.accept(std::move(block), [&](const PcmBlock& aligned) {
      // Preserve the source track even if the bounded mixing queue overruns.
      auto encoded = append_encoded(encoder, aligned, output);
      if (!encoded.is_success()) return encoded;
      if (mixed_encoder_) {
        const auto before = windows_.dropped_frames();
        if (stream == StreamKind::SystemAudio) windows_.push_system(aligned);
        else windows_.push_microphone(aligned);
        mixing_dropped_frames_.fetch_add(windows_.dropped_frames() - before,
                                        std::memory_order_relaxed);
        auto mixed = drain_mix(output);
        if (!mixed.is_success()) return mixed;
      }
      return Result<void>::success();
      });
      if (!accepted.is_success()) return accepted;
    }
    return Result<void>::success();
  }

  Result<void> pump(RecoveringAudioSource& source, PcmNormalizer& normalizer,
                    MfAacEncoder& encoder, StreamKind stream,
                    std::chrono::milliseconds timeout, std::vector<EncodedPacket>& output) {
    auto captured = source.next_block(timeout);
    const auto bit = stream == StreamKind::SystemAudio ? 1U : 2U;
    if (source.recovering()) unavailable_sources_.fetch_or(bit, std::memory_order_relaxed);
    else unavailable_sources_.fetch_and(~bit, std::memory_order_relaxed);
    if (!captured.is_success()) return Result<void>::failure(captured.error());
    if (!captured.value().has_value()) return Result<void>::success();
    auto normalized = normalizer.normalize(std::move(captured.value().value()), stream);
    if (!normalized.is_success()) return Result<void>::failure(normalized.error());
    return accept_blocks(std::move(normalized).value(), encoder, stream, output);
  }

  Result<void> flush_normalizer(PcmNormalizer& normalizer, MfAacEncoder& encoder,
                                StreamKind stream,
                                std::vector<EncodedPacket>& output) {
    auto blocks = normalizer.flush();
    if (!blocks.is_success()) return Result<void>::failure(blocks.error());
    return accept_blocks(std::move(blocks).value(), encoder, stream, output);
  }

  Result<void> drain_mix(std::vector<EncodedPacket>& output, bool draining = false) {
    if (!mixed_encoder_) return Result<void>::success();
    while (true) {
      auto pair = windows_.pop(draining);
      if (!pair.has_value()) return Result<void>::success();
      // Window samples already occupy the same QPC grid. Relabeling timestamps
      // here would hide startup offsets and destroy fractional-tick cadence.
      mixed_pts_ = pair->system.pts;
      auto mixed = mixer_.mix(pair->system, pair->microphone, 1.0F, 1.0F);
      if (!mixed.is_success()) return Result<void>::failure(mixed.error());
      auto encoded = append_encoded(*mixed_encoder_, mixed.value(), output);
      if (!encoded.is_success()) return encoded;
    }
  }

  QpcClock& clock_;
  SynchronizedPcmWindows windows_;
  PcmEpochBridge system_bridge_, microphone_bridge_;
  std::atomic<std::uint32_t> unavailable_sources_{0};
  std::atomic<std::uint64_t> mixing_dropped_frames_{0};
  std::wstring system_audio_id_;
  std::wstring microphone_id_;
  bool system_enabled_;
  bool microphone_enabled_;
  AudioMixer mixer_;
  bool initialized_{false};
  std::optional<QpcTicks> mixed_pts_;
  std::unique_ptr<RecoveringAudioSource> system_source_;
  std::unique_ptr<RecoveringAudioSource> microphone_source_;
  std::unique_ptr<PcmNormalizer> system_normalizer_;
  std::unique_ptr<PcmNormalizer> microphone_normalizer_;
  std::unique_ptr<MfAacEncoder> mixed_encoder_;
  std::unique_ptr<MfAacEncoder> system_encoder_;
  std::unique_ptr<MfAacEncoder> microphone_encoder_;
};

std::vector<StreamDescriptor> audio_descriptors(QpcTicks frequency, bool system = true, bool microphone = true) {
  const auto descriptor = [frequency](StreamKind kind) {
    return StreamDescriptor{kind, "aac", stream_title(kind),
                            {std::byte{0x11}, std::byte{0x90}},
                            {1, static_cast<std::int32_t>(frequency)}, 0, 0, 48'000, 2};
  };
  std::vector<StreamDescriptor> result;
  if (system && microphone) result.push_back(descriptor(StreamKind::MixedAudio));
  if (system) result.push_back(descriptor(StreamKind::SystemAudio));
  if (microphone) result.push_back(descriptor(StreamKind::MicrophoneAudio));
  return result;
}

Result<MonitorInfo> primary_monitor(std::span<const MonitorInfo> monitors) {
  for (const auto& monitor : monitors) if (monitor.primary) return Result<MonitorInfo>::success(monitor);
  return Result<MonitorInfo>::failure(pipeline_error("monitor.primary_missing", "No primary monitor is attached."));
}

struct HostDiagnostics {
  std::string monitor_id;
  std::string os_version;
  std::string gpu;
  std::string driver_version;
  bool h264{false};
  bool nv12{false};
  std::string video_error;
};

std::string dotted_version(DWORD major, DWORD minor, DWORD build, DWORD revision) {
  return std::to_string(major) + "." + std::to_string(minor) + "." +
         std::to_string(build) + "." + std::to_string(revision);
}

HostDiagnostics inspect_host(const std::vector<MonitorInfo>& monitors, QpcClock& clock, std::optional<MonitorId> requested) {
  HostDiagnostics result;
  if (const auto module = GetModuleHandleW(L"ntdll.dll")) {
    using RtlGetVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto get_version = reinterpret_cast<RtlGetVersion>(
        GetProcAddress(module, "RtlGetVersion"));
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (get_version && get_version(&version) == 0) {
      result.os_version = dotted_version(version.dwMajorVersion, version.dwMinorVersion,
                                         version.dwBuildNumber, 0);
    }
  }
  auto selected = select_diagnostic_monitor(requested, monitors);
  if (!selected.is_success()) {
    result.video_error = selected.error().code;
    return result;
  }
  const MonitorInfo* selected_monitor = &selected.value();
  result.monitor_id = monitor_id_text(selected_monitor->id);

  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    result.video_error = "dxgi.factory_creation_failed";
    return result;
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
    if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 description{};
    if (SUCCEEDED(candidate->GetDesc1(&description)) &&
        description.AdapterLuid.HighPart == selected_monitor->id.adapter_luid.HighPart &&
        description.AdapterLuid.LowPart == selected_monitor->id.adapter_luid.LowPart) {
      result.gpu = utf8(description.Description);
      adapter = std::move(candidate);
      break;
    }
  }
  if (adapter) {
    LARGE_INTEGER driver{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) {
      result.driver_version = dotted_version(
          HIWORD(static_cast<DWORD>(driver.HighPart)),
          LOWORD(static_cast<DWORD>(driver.HighPart)),
          HIWORD(driver.LowPart), LOWORD(driver.LowPart));
    }
  }

  auto device = create_d3d11_device(selected_monitor->id.adapter_luid);
  if (!device.is_success()) {
    result.video_error = device.error().code;
    return result;
  }
  NvencConfig config{};
  config.width = static_cast<std::uint32_t>(
      selected_monitor->desktop_rect.right - selected_monitor->desktop_rect.left);
  config.height = static_cast<std::uint32_t>(
      selected_monitor->desktop_rect.bottom - selected_monitor->desktop_rect.top);
  auto encoder = NvencEncoder::create(device.value().device(), config, clock);
  if (!encoder.is_success()) {
    result.video_error = encoder.error().code;
    return result;
  }
  result.h264 = encoder.value()->capabilities().h264;
  result.nv12 = encoder.value()->capabilities().nv12;
  return result;
}

CliExitCode run_list_monitors(DiagnosticWriter& writer, QpcClock& clock) {
  auto monitors = enumerate_monitors();
  if (!monitors.is_success()) { write_error(writer, clock, monitors.error()); return exit_for(monitors.error()); }
  for (const auto& monitor : monitors.value()) {
    writer.write({"monitor", clock.now(), {{"id", monitor_id_text(monitor.id)},
                                            {"adapter_luid", monitor_id_value(monitor.id)},
                                            {"output_index", monitor.id.output_index},
                                            {"name", utf8(monitor.name)},
                                            {"primary", monitor.primary},
                                            {"width", monitor.desktop_rect.right - monitor.desktop_rect.left},
                                            {"height", monitor.desktop_rect.bottom - monitor.desktop_rect.top}}});
  }
  return CliExitCode::Success;
}

CliExitCode run_doctor(DiagnosticWriter& writer, QpcClock& clock, std::optional<MonitorId> requested = std::nullopt) {
  auto monitors = enumerate_monitors();
  auto nvenc = NvencApi::load();
  const auto host = inspect_host(monitors.is_success() ? monitors.value()
                                                       : std::vector<MonitorInfo>{},
                                 clock, requested);
  bool audio_ok = false;
  bool system_audio_ok = false;
  bool microphone_ok = false;
  bool aac_ok = false;
  std::string system_audio_error;
  std::string microphone_error;
  std::string aac_error;
  std::string microphone_message;
  long microphone_hresult = 0;
  if (monitors.is_success() && !monitors.value().empty()) {
    auto system_endpoints = enumerate_system_audio();
    auto microphone_endpoints = enumerate_microphones();
    auto system = system_endpoints.is_success() && !system_endpoints.value().empty()
        ? WasapiCaptureSource::create(
              {AudioEndpointRole::SystemLoopback, system_endpoints.value().front().id}, clock)
        : Result<std::unique_ptr<WasapiCaptureSource>>::failure(
              {"audio.endpoint_unavailable", "No active render endpoint is available.", {}});
    auto microphone = microphone_endpoints.is_success() && !microphone_endpoints.value().empty()
        ? WasapiCaptureSource::create(
              {AudioEndpointRole::Microphone, microphone_endpoints.value().front().id}, clock)
        : Result<std::unique_ptr<WasapiCaptureSource>>::failure(
              {"audio.endpoint_unavailable", "No active capture endpoint is available.", {}});
    auto aac = MfAacEncoder::create(StreamKind::MixedAudio, 192'000, clock);
    system_audio_ok = system.is_success();
    microphone_ok = microphone.is_success();
    aac_ok = aac.is_success();
    if (!system_audio_ok) system_audio_error = system.error().code;
    if (!microphone_ok) {
      microphone_error = microphone.error().code;
      microphone_message = microphone.error().message;
      microphone_hresult = microphone.error().hresult.value_or(0);
    }
    if (!aac_ok) aac_error = aac.error().code;
    audio_ok = system_audio_ok && microphone_ok && aac_ok;
  }
  writer.write({"doctor", clock.now(), {{"windows_x64", sizeof(void*) == 8},
                                         {"os_version", host.os_version},
                                         {"monitor_id", host.monitor_id},
                                         {"recording_color_space", "sdr_bt709"},
                                         {"hdr_fidelity", false},
                                         {"hdr_tone_mapping", false},
                                         {"gpu", host.gpu},
                                         {"driver_version", host.driver_version},
                                         {"monitor_count", monitors.is_success() ? monitors.value().size() : 0},
                                         {"nvenc", nvenc.is_success()},
                                         {"nvenc_max_major", nvenc.is_success() ? nvenc.value().max_supported_version().major : 0},
                                         {"nvenc_max_minor", nvenc.is_success() ? nvenc.value().max_supported_version().minor : 0},
                                         {"h264", host.h264},
                                         {"nv12", host.nv12},
                                         {"video_error", host.video_error},
                                         {"audio_endpoints_and_aac", audio_ok},
                                         {"system_audio", system_audio_ok},
                                         {"system_audio_error", system_audio_error},
                                         {"microphone", microphone_ok},
                                         {"microphone_error", microphone_error},
                                         {"microphone_message", microphone_message},
                                         {"microphone_hresult", microphone_hresult},
                                         {"aac", aac_ok},
                                         {"aac_error", aac_error},
                                         {"libavformat", avformat_version()},
                                         {"libavcodec", avcodec_version()},
                                         {"libavutil", avutil_version()},
                                         {"passed", monitors.is_success() && !monitors.value().empty() && nvenc.is_success() && host.h264 && host.nv12 && audio_ok}}});
  if (!monitors.is_success()) { write_error(writer, clock, monitors.error()); return exit_for(monitors.error()); }
  if (!nvenc.is_success()) { write_error(writer, clock, nvenc.error()); return exit_for(nvenc.error()); }
  if (!host.h264 || !host.nv12) return CliExitCode::UnsupportedHardware;
  if (!audio_ok) return CliExitCode::AudioFailure;
  return CliExitCode::Success;
}

CliExitCode run_recording(const CliArguments& arguments, DiagnosticWriter& writer, QpcClock& qpc) {
  auto monitors_result = enumerate_monitors();
  if (!monitors_result.is_success()) { write_error(writer, qpc, monitors_result.error()); return exit_for(monitors_result.error()); }
  auto monitor = arguments.command == CliCommand::Capture
      ? select_monitor(*arguments.monitor, monitors_result.value())
      : primary_monitor(monitors_result.value());
  if (!monitor.is_success()) { write_error(writer, qpc, monitor.error()); return exit_for(monitor.error()); }
  auto timer = WaitableTimerEngineClock::create();
  if (!timer.is_success()) { write_error(writer, qpc, timer.error()); return exit_for(timer.error()); }
  auto video = HardwareVideoPipeline::create(monitor.value(), arguments, qpc);
  if (!video.is_success()) { write_error(writer, qpc, video.error()); return exit_for(video.error()); }
  auto system_endpoints = enumerate_system_audio();
  if (!system_endpoints.is_success()) {
    write_error(writer, qpc, system_endpoints.error());
    return exit_for(system_endpoints.error());
  }
  auto microphone_endpoints = enumerate_microphones();
  if (!microphone_endpoints.is_success()) {
    write_error(writer, qpc, microphone_endpoints.error());
    return exit_for(microphone_endpoints.error());
  }
  if (system_endpoints.value().empty() || microphone_endpoints.value().empty()) {
    const auto error = pipeline_error(
        "audio.endpoint_unavailable", "An active render and capture endpoint are required.");
    write_error(writer, qpc, error);
    return CliExitCode::AudioFailure;
  }
  const auto system_audio_id = system_endpoints.value().front().id;
  const auto microphone_id = microphone_endpoints.value().front().id;
  auto audio = std::make_shared<HardwareAudioPipeline>(qpc, system_audio_id,
                                                       microphone_id);
  auto hotkey = std::make_shared<RawInputHotkey>();
  auto muxer = std::make_shared<FfmpegClipMuxer>();
  auto continuous_muxer = std::make_shared<FfmpegContinuousMuxer>();
  auto descriptors = audio_descriptors(qpc.frequency());
  descriptors.insert(descriptors.begin(), video.value()->descriptor());
  RecorderEngine engine({.clock = timer.value(),
                         .video = video.value(),
                         .audio = {audio},
                         .hotkey = hotkey,
                         .muxer = muxer,
                         .continuous_muxer = continuous_muxer,
                         .stream_descriptors = descriptors});

  EngineConfig config{};
  config.monitor = monitor.value().id; config.width = arguments.width; config.height = arguments.height;
  config.system_audio_id = system_audio_id;
  config.microphone_id = microphone_id;
  config.fps = arguments.fps; config.bitrate = arguments.bitrate; config.container = arguments.container;
  config.save_replay_hotkey = arguments.hotkey;
  if (arguments.command == CliCommand::Smoke) {
    config.replay_capacity = std::chrono::seconds(arguments.seconds + 2U);
    config.clip_duration = std::chrono::seconds(arguments.seconds);
    config.output_directory = arguments.output.has_parent_path() ? arguments.output.parent_path() : std::filesystem::current_path();
  } else {
    config.replay_capacity = std::chrono::seconds(arguments.buffer_seconds);
    config.clip_duration = std::chrono::seconds(arguments.clip_seconds);
    config.output_directory = arguments.output;
  }
  std::error_code directory_error;
  std::filesystem::create_directories(config.output_directory, directory_error);
  if (directory_error) {
    const auto error = pipeline_error("mux.output_directory_failed", "Could not create the output directory.");
    write_error(writer, qpc, error); return CliExitCode::MuxFailure;
  }
  auto started = engine.start(config);
  if (!started.is_success()) { write_error(writer, qpc, started.error()); return exit_for(started.error()); }
  writer.write({"recording_started", qpc.now(), {{"monitor", monitor_id_text(monitor.value().id)},
                                                  {"fps", config.fps}, {"width", config.width}, {"height", config.height}}});

  if (arguments.command == CliCommand::Smoke) {
    std::this_thread::sleep_for(std::chrono::seconds(arguments.seconds));
    auto saved = engine.save_clip(qpc.now());
    auto saved_result = saved.get();
    auto stopped = engine.stop();
    if (!stopped.is_success()) { write_error(writer, qpc, stopped.error()); return exit_for(stopped.error()); }
    if (!saved_result.is_success()) { write_error(writer, qpc, saved_result.error()); return exit_for(saved_result.error()); }
    if (saved_result.value() != arguments.output) {
      if (std::filesystem::exists(arguments.output) ||
          MoveFileExW(saved_result.value().c_str(), arguments.output.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
        const auto error = pipeline_error("mux.final_rename_failed", "Could not move the smoke clip to the requested output.", static_cast<long>(HRESULT_FROM_WIN32(GetLastError())));
        write_error(writer, qpc, error); return CliExitCode::MuxFailure;
      }
    }
    const auto metrics = engine.metrics();
    writer.write({"smoke_completed", qpc.now(), {{"video_packets", metrics.video_packets},
                                                  {"audio_packets", metrics.audio_packets},
                                                  {"completed_saves", metrics.completed_saves}}});
    return CliExitCode::Success;
  }

  stop_requested = false;
  SetConsoleCtrlHandler(console_control, TRUE);
  const auto automatic_stop = arguments.duration_seconds.has_value()
      ? std::optional(std::chrono::steady_clock::now() +
                      std::chrono::seconds(*arguments.duration_seconds))
      : std::nullopt;
  while (!stop_requested.load() &&
         (!automatic_stop.has_value() || std::chrono::steady_clock::now() < *automatic_stop)) {
    std::this_thread::sleep_for(1s);
    const auto metrics = engine.metrics();
    writer.write({"metrics", qpc.now(),
                  engine_metrics_json(metrics, qpc.frequency(), video.value()->metrics())});
    if (metrics.pipeline_errors != 0) break;
  }
  SetConsoleCtrlHandler(console_control, FALSE);
  auto stopped = engine.stop();
  if (!stopped.is_success()) { write_error(writer, qpc, stopped.error()); return exit_for(stopped.error()); }
  return CliExitCode::Success;
}

}  // namespace

Result<RecorderEngineDependencies> HardwarePipelineFactory::create(const EngineConfig& config) {
  auto monitors=enumerate_monitors();
  if(!monitors.is_success()) return Result<RecorderEngineDependencies>::failure(monitors.error());
  auto monitor=select_monitor(config.monitor,monitors.value());
  if(!monitor.is_success()) return Result<RecorderEngineDependencies>::failure(monitor.error());
  auto check=[](bool enabled,const std::wstring& id, bool system)->Result<void> {
    if(!enabled) return Result<void>::success();
    auto endpoints=system?enumerate_system_audio():enumerate_microphones();
    if(!endpoints.is_success()) return Result<void>::failure(endpoints.error());
    for(const auto& endpoint:endpoints.value()) if(endpoint.id==id && !id.empty()) return Result<void>::success();
    return Result<void>::failure(pipeline_error("audio.endpoint_unavailable","The pinned endpoint is no longer active; reselect it explicitly."));
  };
  auto system=check(config.system_audio_enabled,config.system_audio_id,true);
  if(!system.is_success()) return Result<RecorderEngineDependencies>::failure(system.error());
  auto microphone=check(config.microphone_enabled,config.microphone_id,false);
  if(!microphone.is_success()) return Result<RecorderEngineDependencies>::failure(microphone.error());
  CliArguments arguments;
  arguments.width=config.width; arguments.height=config.height;
  arguments.fps=config.fps; arguments.bitrate=config.bitrate;
  auto video=HardwareVideoPipeline::create(monitor.value(),arguments,clock_);
  if(!video.is_success()) return Result<RecorderEngineDependencies>::failure(video.error());
  auto timer=WaitableTimerEngineClock::create();
  if(!timer.is_success()) return Result<RecorderEngineDependencies>::failure(timer.error());
  auto descriptors=audio_descriptors(clock_.frequency(),config.system_audio_enabled,config.microphone_enabled);
  descriptors.insert(descriptors.begin(),video.value()->descriptor());
  std::vector<std::shared_ptr<IEngineAudioPipeline>> audio;
  if(config.system_audio_enabled || config.microphone_enabled)
    audio.push_back(std::make_shared<HardwareAudioPipeline>(clock_,config.system_audio_id,config.microphone_id,
        config.system_audio_enabled,config.microphone_enabled));
  auto categories = GameCategoryResolver::create(monitor.value(), config.output_directory);
  return Result<RecorderEngineDependencies>::success({.clock=timer.value(),.video=video.value(),
    .audio=std::move(audio),.hotkey=std::make_shared<RawInputHotkey>(),.muxer=std::make_shared<FfmpegClipMuxer>(),
    .continuous_muxer=std::make_shared<FfmpegContinuousMuxer>(),.stream_descriptors=std::move(descriptors),
    .capture_category=[categories] { return categories->foreground_folder(); },
    .cache_category_icon=[categories](const std::filesystem::path& category) { categories->cache_folder_icon(category); }});
}
Result<nlohmann::json> HardwarePipelineFactory::catalog() {
  auto monitors=enumerate_monitors();
  if(!monitors.is_success()) return Result<nlohmann::json>::failure(monitors.error());
  auto system=enumerate_system_audio();
  if(!system.is_success()) return Result<nlohmann::json>::failure(system.error());
  auto microphones=enumerate_microphones();
  if(!microphones.is_success()) return Result<nlohmann::json>::failure(microphones.error());
  nlohmann::json result{{"protocolVersion",1},{"type","catalog"},{"monitors",nlohmann::json::array()},
    {"systemAudio",nlohmann::json::array()},{"microphones",nlohmann::json::array()}};
  for(const auto& monitor:monitors.value()) result["monitors"].push_back({
    {"id",monitor_id_text(monitor.id)},{"legacyId",std::to_string(monitor_id_value(monitor.id))+":"+std::to_string(monitor.id.output_index)},
    {"name",utf8(monitor.name)},{"primary",monitor.primary},
    {"width",monitor.desktop_rect.right-monitor.desktop_rect.left},{"height",monitor.desktop_rect.bottom-monitor.desktop_rect.top}});
  for(const auto& endpoint:system.value()) result["systemAudio"].push_back({
    {"id",utf8(endpoint.id)},{"name",utf8(endpoint.name)},{"defaultConsole",endpoint.default_console}});
  for(const auto& endpoint:microphones.value()) result["microphones"].push_back({
    {"id",utf8(endpoint.id)},{"name",utf8(endpoint.name)},{"defaultConsole",endpoint.default_console}});
  return Result<nlohmann::json>::success(std::move(result));
}
nlohmann::json HardwarePipelineFactory::doctor(std::optional<MonitorId> monitor) {
  std::ostringstream stream; DiagnosticWriter writer(stream);
  const auto status=run_doctor(writer,clock_,monitor);
  std::istringstream lines(stream.str()); std::string line;
  nlohmann::json result{{"protocolVersion",1},{"type","doctor"},{"passed",false}};
  while(std::getline(lines,line)) {
    auto item=nlohmann::json::parse(line);
    if(item.contains("fields")) result["details"]=item["fields"];
    else result["details"]=item;
    if(result["details"].contains("passed")) break;
  }
  result["passed"]=status==CliExitCode::Success;
  return result;
}
CliExitCode HardwarePipelineFactory::run_legacy(const CliArguments& arguments,DiagnosticWriter& writer) {
  ComApartment apartment;
  if(!apartment.initialized()) return CliExitCode::UnsupportedHardware;
  switch(arguments.command) {
    case CliCommand::Doctor: return run_doctor(writer,clock_,arguments.monitor);
    case CliCommand::ListMonitors: return run_list_monitors(writer,clock_);
    case CliCommand::Capture:
    case CliCommand::Smoke: return run_recording(arguments,writer,clock_);
    case CliCommand::ListAudioDevices: {
      auto result=catalog(); if(!result.is_success()) return exit_for(result.error());
      writer.write({"audio_devices",clock_.now(),result.value()}); return CliExitCode::Success;
    }
    default: return CliExitCode::InvalidArguments;
  }
}
} // namespace rebelliocap
