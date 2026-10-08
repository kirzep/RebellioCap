#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "audio/mf_aac_encoder.h"
#include "engine/continuous_recording.h"
#include "mux/ffmpeg_continuous_muxer.h"
#include "replay/replay_ring.h"
#include "video/d3d11_nv12_converter.h"
#include "video/nvenc_encoder.h"

namespace {

using Microsoft::WRL::ComPtr;
using namespace rebelliocap;

bool check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

std::filesystem::path artifact_directory() {
  const auto identity = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::current_path() / "artifacts" /
      ("task-3-continuous-" + std::to_string(identity));
  std::filesystem::create_directories(directory);
  return directory;
}

}  // namespace

int main() {
  QpcClock clock;
  const auto origin = clock.frequency() * 10;
  const auto frame_duration = clock.frequency() / 60;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    std::cout << "not-run: DXGI factory unavailable\n";
    return 77;
  }
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> candidate;
    if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 descriptor{};
    if (SUCCEEDED(candidate->GetDesc1(&descriptor)) && descriptor.VendorId == 0x10de) {
      adapter = std::move(candidate);
      break;
    }
  }
  if (!adapter) {
    std::cout << "not-run: NVIDIA adapter unavailable\n";
    return 77;
  }

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &device, nullptr, &context))) {
    std::cout << "not-run: D3D11 device unavailable\n";
    return 77;
  }
  constexpr UINT width = 640;
  constexpr UINT height = 360;
  D3D11_TEXTURE2D_DESC texture_descriptor{};
  texture_descriptor.Width = width;
  texture_descriptor.Height = height;
  texture_descriptor.MipLevels = 1;
  texture_descriptor.ArraySize = 1;
  texture_descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  texture_descriptor.SampleDesc.Count = 1;
  texture_descriptor.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  CapturedVideoFrame frame{};
  frame.width = width;
  frame.height = height;
  frame.captured_at = origin;
  frame.lifetime = std::make_shared<int>(1);
  if (FAILED(device->CreateTexture2D(&texture_descriptor, nullptr, &frame.texture))) {
    std::cout << "not-run: test texture unavailable\n";
    return 77;
  }
  ComPtr<ID3D11RenderTargetView> target;
  if (FAILED(device->CreateRenderTargetView(frame.texture.Get(), nullptr, &target))) {
    std::cout << "not-run: render target unavailable\n";
    return 77;
  }
  auto converted = D3d11Nv12Converter::create(
      device.Get(), context.Get(), {width, height, width, height, 8});
  if (!converted.is_success()) {
    std::cout << "not-run: " << converted.error().code << ": "
              << converted.error().message << '\n';
    return 77;
  }
  auto converter = std::move(converted).value();
  auto video_created = NvencEncoder::create(
      device.Get(), {width, height, 60, 3'000'000, 2}, clock);
  if (!video_created.is_success()) {
    std::cout << "not-run: " << video_created.error().code << ": "
              << video_created.error().message << '\n';
    return 77;
  }
  auto video = std::move(video_created).value();
  std::vector<StreamDescriptor> descriptors{video->descriptor()};
  std::vector<EncodedPacket> packets;
  for (int index = 0; index < 60; ++index) {
    const float color[]{static_cast<float>(index) / 80.0F, 0.15F, 0.35F, 1.0F};
    context->ClearRenderTargetView(target.Get(), color);
    auto nv12 = converter->convert(frame);
    if (!check(nv12.is_success(), "NV12 conversion failed")) return 1;
    auto output = video->encode(std::move(nv12).value(),
                                origin + frame_duration * index, index == 0);
    if (!check(output.is_success(), "NVENC encode failed")) return 1;
    for (auto& packet : output.value()) packets.push_back(std::move(packet));
  }
  auto video_tail = video->flush();
  if (!check(video_tail.is_success(), "NVENC flush failed")) return 1;
  for (auto& packet : video_tail.value()) packets.push_back(std::move(packet));

  constexpr std::array audio_kinds{StreamKind::MixedAudio,
                                    StreamKind::SystemAudio,
                                    StreamKind::MicrophoneAudio};
  constexpr std::array bitrates{192'000U, 160'000U, 128'000U};
  for (std::size_t stream = 0; stream < audio_kinds.size(); ++stream) {
    auto created = MfAacEncoder::create(audio_kinds[stream], bitrates[stream], clock);
    if (!created.is_success()) {
      std::cout << "not-run: " << created.error().code << ": "
                << created.error().message << '\n';
      return 77;
    }
    auto encoder = std::move(created).value();
    descriptors.push_back(encoder->descriptor());
    for (std::uint64_t audio_frame = 0; audio_frame < 48'000; audio_frame += 480) {
      PcmBlock block{audio_kinds[stream],
                     origin + static_cast<QpcTicks>(audio_frame) * clock.frequency() / 48'000,
                     48'000,
                     2,
                     std::vector<float>(960),
                     0};
      for (std::size_t sample = 0; sample < 480; ++sample) {
        const auto value = 0.15F * static_cast<float>(std::sin(
            6.283185307179586 * (330.0 + 110.0 * stream) *
            static_cast<double>(audio_frame + sample) / 48'000.0));
        block.interleaved[sample * 2] = value;
        block.interleaved[sample * 2 + 1] = value;
      }
      auto output = encoder->encode(block);
      if (!check(output.is_success(), "AAC encode failed")) return 1;
      for (auto& packet : output.value()) packets.push_back(std::move(packet));
    }
    auto tail = encoder->flush();
    if (!check(tail.is_success(), "AAC flush failed")) return 1;
    for (auto& packet : tail.value()) packets.push_back(std::move(packet));
  }
  std::stable_sort(packets.begin(), packets.end(), [](const auto& left, const auto& right) {
    return left.dts < right.dts;
  });

  ReplayRing replay(clock.frequency() * 30);
  FfmpegContinuousMuxer muxer;
  ContinuousRecording recording(muxer, descriptors);
  const auto destination = artifact_directory() / "continuous.mp4";
  auto started = recording.start(destination, Container::Mp4);
  if (!check(started.is_success(), "Continuous mux open failed")) return 1;
  std::size_t replay_packets = 0;
  for (const auto& packet : packets) {
    auto appended = replay.append(packet);
    if (!check(appended.is_success(), "Replay append failed")) return 1;
    ++replay_packets;
    auto accepted = recording.accept(packet);
    if (!check(accepted.is_success(), "Continuous packet accept failed")) return 1;
  }
  auto stopped = recording.stop();
  if (!stopped.is_success()) {
    std::cerr << stopped.error().code << ": " << stopped.error().message << '\n';
    return 1;
  }
  if (!check(std::filesystem::exists(destination), "Final media file is missing") ||
      !check(!std::filesystem::exists(destination.wstring() + L".partial"),
             "Owned partial file survived finalization") ||
      !check(recording.packets_written() == packets.size(),
             "Continuous packet count mismatch") ||
      !check(replay_packets == packets.size(), "Replay did not receive every packet")) {
    return 1;
  }

  AVFormatContext* input = nullptr;
  if (avformat_open_input(&input, destination.string().c_str(), nullptr, nullptr) < 0) {
    std::cerr << "ffprobe-equivalent container open failed\n";
    return 1;
  }
  const auto close_input = [&] { avformat_close_input(&input); };
  if (avformat_find_stream_info(input, nullptr) < 0) {
    close_input();
    std::cerr << "stream discovery failed\n";
    return 1;
  }
  std::size_t h264_streams = 0;
  std::size_t aac_streams = 0;
  bool time_bases_match_replay = true;
  for (unsigned index = 0; index < input->nb_streams; ++index) {
    const auto* stream = input->streams[index];
    h264_streams += stream->codecpar->codec_id == AV_CODEC_ID_H264;
    aac_streams += stream->codecpar->codec_id == AV_CODEC_ID_AAC;
    if (stream->codecpar->codec_id == AV_CODEC_ID_H264) {
      time_bases_match_replay = time_bases_match_replay &&
          stream->time_base.num == 1 && stream->time_base.den == 90'000;
    } else if (stream->codecpar->codec_id == AV_CODEC_ID_AAC) {
      time_bases_match_replay = time_bases_match_replay &&
          stream->time_base.num == 1 && stream->time_base.den == 48'000;
    }
  }
  std::size_t demuxed_packets = 0;
  AVPacket* packet = av_packet_alloc();
  while (packet != nullptr && av_read_frame(input, packet) >= 0) {
    ++demuxed_packets;
    av_packet_unref(packet);
  }
  av_packet_free(&packet);
  close_input();
  if (!check(h264_streams == 1, "Expected one H.264 stream") ||
      !check(aac_streams == 3, "Expected three enabled AAC streams") ||
      !check(time_bases_match_replay, "Continuous stream time bases differ from Replay") ||
      !check(demuxed_packets == packets.size(), "Demuxed packet count mismatch")) {
    return 1;
  }
  std::cout << "CONTINUOUS_ARTIFACT=" << destination.string()
            << " h264_streams=" << h264_streams
            << " aac_streams=" << aac_streams
            << " packets=" << demuxed_packets
            << " replay_packets=" << replay_packets << '\n';
  return 0;
}
