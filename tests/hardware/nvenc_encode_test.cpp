#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <wrl/client.h>

#include "capture/capture_source.h"
#include "core/qpc_clock.h"
#include "video/d3d11_nv12_converter.h"
#include "video/nvenc_api.h"
#include "video/nvenc_encoder.h"

namespace {

constexpr int kSkipped = 77;
constexpr std::uint32_t kWidth = 1920;
constexpr std::uint32_t kHeight = 1080;
constexpr std::uint32_t kFps = 60;
constexpr std::uint32_t kBitrate = 30'000'000;
constexpr std::uint32_t kMaxBitrate = 45'000'000;
constexpr std::uint32_t kGopSeconds = 2;
constexpr std::size_t kPoolSize = 8;
constexpr std::uint32_t kFunctionalFrames = (kFps * kGopSeconds * 2U) + 5U;
constexpr std::uint64_t kTimestampDigestOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kTimestampDigestPrime = 1'099'511'628'211ULL;

int fail(const std::string& reason) {
  std::cerr << "FAIL: " << reason << '\n';
  return 1;
}

bool is_precisely_unavailable(const std::string& code) {
  return code == "nvenc.library_unavailable" ||
         code == "nvenc.driver_api_too_old" ||
         code == "nvenc.no_encode_device" ||
         code == "nvenc.unsupported_device" ||
         code == "nvenc.h264_unavailable";
}

struct TestDevice {
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  DXGI_ADAPTER_DESC1 description{};
};

rebelliocap::Result<TestDevice> create_nvidia_device() {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(result)) {
    return rebelliocap::Result<TestDevice>::failure(rebelliocap::Error{
        "nvenc_test.factory_failed", "Could not create the DXGI factory.",
        static_cast<long>(result)});
  }

  TestDevice selected;
  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    result = factory->EnumAdapters1(index, &adapter);
    if (result == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(result)) {
      return rebelliocap::Result<TestDevice>::failure(rebelliocap::Error{
          "nvenc_test.adapter_enumeration_failed", "Could not enumerate DXGI adapters.",
          static_cast<long>(result)});
    }
    DXGI_ADAPTER_DESC1 description{};
    result = adapter->GetDesc1(&description);
    if (FAILED(result)) {
      return rebelliocap::Result<TestDevice>::failure(rebelliocap::Error{
          "nvenc_test.adapter_description_failed", "Could not inspect a DXGI adapter.",
          static_cast<long>(result)});
    }
    if (description.VendorId == 0x10DEU &&
        (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
      selected.adapter = std::move(adapter);
      selected.description = description;
      break;
    }
  }
  if (selected.adapter == nullptr) {
    return rebelliocap::Result<TestDevice>::failure(rebelliocap::Error{
        "nvenc.no_encode_device", "No NVIDIA hardware DXGI adapter is available.",
        std::nullopt});
  }

  constexpr std::array feature_levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  result = D3D11CreateDevice(
      selected.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels.data(),
      static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION, &selected.device,
      nullptr, &selected.context);
  if (result == E_INVALIDARG) {
    result = D3D11CreateDevice(
        selected.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels.data() + 1, 1,
        D3D11_SDK_VERSION, &selected.device, nullptr, &selected.context);
  }
  if (FAILED(result)) {
    return rebelliocap::Result<TestDevice>::failure(rebelliocap::Error{
        "nvenc_test.device_creation_failed",
        "Could not create a D3D11 device on the NVIDIA adapter.",
        static_cast<long>(result)});
  }
  return rebelliocap::Result<TestDevice>::success(std::move(selected));
}

rebelliocap::Result<rebelliocap::CapturedVideoFrame> make_bgra_frame(
    ID3D11Device* device, ID3D11DeviceContext* context) {
  D3D11_TEXTURE2D_DESC description{};
  description.Width = kWidth;
  description.Height = kHeight;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  HRESULT result = device->CreateTexture2D(&description, nullptr, &texture);
  if (FAILED(result)) {
    return rebelliocap::Result<rebelliocap::CapturedVideoFrame>::failure(
        rebelliocap::Error{"nvenc_test.input_texture_failed",
                            "Could not create the GPU BGRA test texture.",
                            static_cast<long>(result)});
  }
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
  result = device->CreateRenderTargetView(texture.Get(), nullptr, &target);
  if (FAILED(result)) {
    return rebelliocap::Result<rebelliocap::CapturedVideoFrame>::failure(
        rebelliocap::Error{"nvenc_test.input_view_failed",
                            "Could not create the GPU BGRA render-target view.",
                            static_cast<long>(result)});
  }
  constexpr std::array<float, 4> initial_color{0.1F, 0.2F, 0.3F, 1.0F};
  context->ClearRenderTargetView(target.Get(), initial_color.data());

  rebelliocap::CapturedVideoFrame frame;
  frame.texture = std::move(texture);
  frame.captured_at = 1;
  frame.width = kWidth;
  frame.height = kHeight;
  frame.lifetime = std::make_shared<const int>(1);
  return rebelliocap::Result<rebelliocap::CapturedVideoFrame>::success(std::move(frame));
}

std::uint32_t soak_seconds() {
  std::array<wchar_t, 32> value{};
  const DWORD length = GetEnvironmentVariableW(
      L"REBELLIOCAP_NVENC_SOAK_SECONDS", value.data(),
      static_cast<DWORD>(value.size()));
  if (length == 0) {
    return 600;
  }
  try {
    const auto parsed = std::stoul(value.data());
    if (parsed == 0 || parsed > 3600) {
      return 600;
    }
    return static_cast<std::uint32_t>(parsed);
  } catch (...) {
    return 600;
  }
}

struct ProcessSample {
  std::uint64_t handles;
  std::uint64_t private_bytes;
};

std::optional<ProcessSample> process_sample() {
  DWORD handles = 0;
  if (GetProcessHandleCount(GetCurrentProcess(), &handles) == FALSE) {
    return std::nullopt;
  }
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters)) == FALSE) {
    return std::nullopt;
  }
  return ProcessSample{handles, static_cast<std::uint64_t>(counters.PrivateUsage)};
}

bool has_annex_b_start_code(const std::vector<std::byte>& bytes) {
  if (bytes.size() < 4) {
    return false;
  }
  for (std::size_t index = 0; index + 3 < bytes.size(); ++index) {
    const auto first = std::to_integer<unsigned char>(bytes[index]);
    const auto second = std::to_integer<unsigned char>(bytes[index + 1]);
    const auto third = std::to_integer<unsigned char>(bytes[index + 2]);
    if (first == 0 && second == 0 && third == 1) {
      return true;
    }
    if (index + 4 <= bytes.size() && first == 0 && second == 0 && third == 0 &&
        std::to_integer<unsigned char>(bytes[index + 3]) == 1) {
      return true;
    }
  }
  return false;
}

std::optional<std::uint8_t> h264_sps_profile_idc(const std::vector<std::byte>& bytes) {
  for (std::size_t index = 0; index + 5 < bytes.size(); ++index) {
    std::size_t nal = 0;
    if (std::to_integer<unsigned char>(bytes[index]) == 0 &&
        std::to_integer<unsigned char>(bytes[index + 1]) == 0 &&
        std::to_integer<unsigned char>(bytes[index + 2]) == 1) {
      nal = index + 3;
    } else if (std::to_integer<unsigned char>(bytes[index]) == 0 &&
               std::to_integer<unsigned char>(bytes[index + 1]) == 0 &&
               std::to_integer<unsigned char>(bytes[index + 2]) == 0 &&
               std::to_integer<unsigned char>(bytes[index + 3]) == 1) {
      nal = index + 4;
    }
    if (nal != 0 && nal + 1 < bytes.size() &&
        (std::to_integer<unsigned char>(bytes[nal]) & 0x1FU) == 7U) {
      return std::to_integer<std::uint8_t>(bytes[nal + 1]);
    }
  }
  return std::nullopt;
}

bool monotonic_growth(const std::vector<ProcessSample>& samples,
                      bool handles) {
  if (samples.size() < 4) {
    return false;
  }
  std::size_t increases = 0;
  bool never_decreased = true;
  for (std::size_t index = 1; index < samples.size(); ++index) {
    const auto previous = handles ? samples[index - 1].handles
                                  : samples[index - 1].private_bytes;
    const auto current = handles ? samples[index].handles
                                 : samples[index].private_bytes;
    never_decreased = never_decreased && current >= previous;
    increases += current > previous ? 1U : 0U;
  }
  const auto first = handles ? samples.front().handles : samples.front().private_bytes;
  const auto last = handles ? samples.back().handles : samples.back().private_bytes;
  const std::uint64_t material_growth = handles ? 2U : (1024U * 1024U);
  return never_decreased && increases >= 3 && last > first + material_growth;
}

std::uint64_t digest_timestamp(std::uint64_t digest,
                               rebelliocap::QpcTicks timestamp) {
  std::uint64_t bits = static_cast<std::uint64_t>(timestamp);
  for (std::size_t byte = 0; byte < sizeof(bits); ++byte) {
    digest ^= bits & 0xffU;
    digest *= kTimestampDigestPrime;
    bits >>= 8U;
  }
  return digest;
}

}  // namespace

int main() {
  using namespace rebelliocap;

  auto api_result = NvencApi::load();
  if (!api_result.is_success()) {
    if (is_precisely_unavailable(api_result.error().code)) {
      std::cerr << "SKIP: " << api_result.error().code << ": "
                << api_result.error().message << '\n';
      return kSkipped;
    }
    return fail("NVENC runtime loader failed: " + api_result.error().code);
  }
  auto api = std::move(api_result).value();
  if (!api.has_required_functions()) {
    return fail("NVENC function table omitted a required entry");
  }
  const auto runtime_version = api.max_supported_version();
  const auto required_version = api.required_version();
  if (runtime_version.packed < required_version.packed ||
      required_version.major != 13 || required_version.minor != 0) {
    return fail("NVENC API 13.0 negotiation was not enforced");
  }

  auto device_result = create_nvidia_device();
  if (!device_result.is_success()) {
    if (is_precisely_unavailable(device_result.error().code)) {
      std::cerr << "SKIP: " << device_result.error().code << ": "
                << device_result.error().message << '\n';
      return kSkipped;
    }
    return fail("NVIDIA D3D11 device creation failed: " + device_result.error().code);
  }
  auto test_device = std::move(device_result).value();

  auto input_result = make_bgra_frame(test_device.device.Get(), test_device.context.Get());
  if (!input_result.is_success()) {
    return fail(input_result.error().message);
  }
  auto input = std::move(input_result).value();

  D3d11Nv12ConverterConfig converter_config{
      kWidth, kHeight, kWidth, kHeight, kPoolSize};
  auto converter_result = D3d11Nv12Converter::create(
      test_device.device.Get(), test_device.context.Get(), converter_config);
  if (!converter_result.is_success()) {
    return fail("NV12 converter creation failed: " + converter_result.error().code);
  }
  auto converter = std::move(converter_result).value();

  QpcClock clock;
  NvencConfig config;
  config.width = kWidth;
  config.height = kHeight;
  config.fps = kFps;
  config.bitrate = kBitrate;
  config.gop_seconds = kGopSeconds;
  auto encoder_result = NvencEncoder::create(test_device.device.Get(), config, clock);
  if (!encoder_result.is_success()) {
    if (is_precisely_unavailable(encoder_result.error().code)) {
      std::cerr << "SKIP: " << encoder_result.error().code << ": "
                << encoder_result.error().message << '\n';
      return kSkipped;
    }
    return fail("NVENC encoder creation failed: " + encoder_result.error().code +
                ": " + encoder_result.error().message);
  }
  auto encoder = std::move(encoder_result).value();
  const auto lifetime = encoder->lifetime_metrics();
  const auto capabilities = encoder->capabilities();
  const auto accepted = encoder->accepted_config();
  if (!capabilities.h264 || !capabilities.nv12 || !capabilities.async_encode ||
      capabilities.max_b_frames < 2 ||
      (capabilities.rate_control_modes & 1U) == 0 ||
      capabilities.min_width > kWidth || capabilities.max_width < kWidth ||
      capabilities.min_height > kHeight || capabilities.max_height < kHeight) {
    return fail("queried NVENC capabilities do not prove the requested baseline");
  }
  if (accepted.width != kWidth || accepted.height != kHeight ||
      accepted.fps != kFps || accepted.target_bitrate != kBitrate ||
      accepted.max_bitrate != kMaxBitrate ||
      accepted.gop_frames > kFps * kGopSeconds ||
      accepted.frame_interval_p != 3 || accepted.lookahead_depth != 0 ||
      accepted.spatial_aq_enabled || !accepted.high_profile ||
      !accepted.preset_p5 || !accepted.high_quality_tuning || !accepted.vbr) {
    return fail("driver-accepted request differs from the Task 7 baseline");
  }

  const QpcTicks frame_duration = clock.frequency() / static_cast<QpcTicks>(kFps);
  if (frame_duration <= 0) {
    return fail("QPC frequency cannot express a 60 FPS duration");
  }
  const QpcTicks first_pts = clock.now();
  std::uint64_t submitted = 0;
  std::uint64_t packet_count = 0;
  std::optional<QpcTicks> first_packet_dts;
  std::optional<QpcTicks> last_packet_pts;
  std::optional<QpcTicks> last_packet_dts;
  std::optional<std::uint8_t> profile_idc;
  std::size_t idr_count = 0;
  std::optional<QpcTicks> last_idr_pts;
  std::uint64_t maximum_idr_gap = 0;
  bool reordered_pts_observed = false;
  std::uint64_t packet_pts_digest = kTimestampDigestOffset;

  const auto observe_packets =
      [&](std::vector<EncodedPacket> ready) -> std::optional<std::string> {
    for (const auto& packet : ready) {
      if (packet.stream != StreamKind::Video || packet.payload == nullptr ||
          packet.epoch != 0 || packet.payload->empty() ||
          !has_annex_b_start_code(*packet.payload)) {
        return "an encoded packet is empty or not Annex B H.264";
      }
      if (packet.duration != frame_duration || packet.dts > packet.pts ||
          (last_packet_dts.has_value() && packet.dts <= *last_packet_dts)) {
        return "encoded packet DTS is not a valid monotonic QPC decode timeline";
      }
      first_packet_dts = first_packet_dts.has_value()
                             ? first_packet_dts
                             : std::optional<QpcTicks>(packet.dts);
      reordered_pts_observed =
          reordered_pts_observed ||
          (last_packet_pts.has_value() && packet.pts < *last_packet_pts);
      packet_pts_digest = digest_timestamp(packet_pts_digest, packet.pts);
      last_packet_pts = packet.pts;
      last_packet_dts = packet.dts;
      if (packet.keyframe) {
        ++idr_count;
        if (last_idr_pts.has_value()) {
          if (packet.pts <= *last_idr_pts) {
            return "IDR presentation timestamps moved backward";
          }
          maximum_idr_gap = (std::max)(maximum_idr_gap,
                                       static_cast<std::uint64_t>(
                                           (packet.pts - *last_idr_pts) /
                                           frame_duration));
        }
        last_idr_pts = packet.pts;
        if (!profile_idc.has_value()) {
          profile_idc = h264_sps_profile_idc(*packet.payload);
        }
      }
      ++packet_count;
    }
    return std::nullopt;
  };

  const auto submit_one = [&](bool force_keyframe) -> std::optional<std::string> {
    auto conversion = converter->convert(input);
    if (!conversion.is_success()) {
      return "NV12 conversion failed during encode: " + conversion.error().code;
    }
    const auto pts = first_pts + static_cast<QpcTicks>(submitted) * frame_duration;
    auto encoded = encoder->encode(std::move(conversion).value(), pts, force_keyframe);
    if (!encoded.is_success()) {
      return "NVENC submission failed: " + encoded.error().code + ": " +
             encoded.error().message;
    }
    auto ready = std::move(encoded).value();
    if (const auto error = observe_packets(std::move(ready)); error.has_value()) {
      return error;
    }
    ++submitted;
    return std::nullopt;
  };

  bool async_lease_retention_proved = false;
  for (std::uint32_t frame = 0; frame < kFunctionalFrames; ++frame) {
    if (const auto error = submit_one(false); error.has_value()) {
      return fail(*error);
    }
    if (frame == 1U) {
      async_lease_retention_proved = lifetime->snapshot().in_flight > 0;
      if (!async_lease_retention_proved) {
        return fail("B-frame submission did not retain an asynchronous input lease");
      }
    }
  }

  std::vector<ProcessSample> process_samples;
  if (const auto sample = process_sample(); sample.has_value()) {
    process_samples.push_back(*sample);
  } else {
    return fail("could not sample process handles/private bytes");
  }
  const auto soak_start = std::chrono::steady_clock::now();
  auto next_frame = soak_start;
  auto next_sample = soak_start + std::chrono::seconds(5);
  const auto soak_end = soak_start + std::chrono::seconds(soak_seconds());
  while (std::chrono::steady_clock::now() < soak_end) {
    next_frame += std::chrono::nanoseconds(1'000'000'000LL / kFps);
    if (const auto error = submit_one(false); error.has_value()) {
      return fail(*error);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_sample) {
      const auto sample = process_sample();
      if (!sample.has_value()) {
        return fail("process resource sampling failed during soak");
      }
      process_samples.push_back(*sample);
      next_sample += std::chrono::seconds(5);
    }
    std::this_thread::sleep_until(next_frame);
  }

  auto flushed = encoder->flush();
  if (!flushed.is_success()) {
    return fail("NVENC flush failed: " + flushed.error().code);
  }
  auto tail = std::move(flushed).value();
  if (const auto error = observe_packets(std::move(tail)); error.has_value()) {
    return fail(*error);
  }
  const auto final_sample = process_sample();
  if (!final_sample.has_value()) {
    return fail("final process resource sampling failed");
  }
  process_samples.push_back(*final_sample);

  if (packet_count != submitted || packet_count == 0) {
    return fail("NVENC did not return exactly one nonempty packet per submitted frame");
  }
  if (!first_packet_dts.has_value() ||
      *first_packet_dts != first_pts - (2 * frame_duration)) {
    return fail("decode timeline does not include the two-B-frame initial offset");
  }
  if (!reordered_pts_observed) {
    return fail("driver output did not expose nontrivial B-frame PTS reordering");
  }
  if (!profile_idc.has_value() || *profile_idc != 100U) {
    return fail("emitted SPS does not prove H.264 High profile_idc 100");
  }
  if (idr_count < 2) {
    return fail("fewer than two IDR packets were emitted");
  }
  if (maximum_idr_gap > kFps * kGopSeconds) {
    return fail("IDR cadence exceeded the configured two-second GOP");
  }

  const auto metrics = encoder->metrics();
  if (metrics.submitted_frames != submitted || metrics.completed_frames != submitted ||
      metrics.in_flight_frames != 0 || metrics.p50_latency_us == 0 ||
      metrics.p95_latency_us < metrics.p50_latency_us ||
      metrics.p_picture_frames == 0 || metrics.b_picture_frames == 0 ||
      metrics.driver_timestamp_packets != packet_count ||
      metrics.driver_timestamp_digest != packet_pts_digest) {
    return fail("encoder latency/completion metrics are incomplete");
  }
  if (monotonic_growth(process_samples, true) ||
      monotonic_growth(process_samples, false)) {
    return fail("10-minute soak detected monotonic process resource growth");
  }

  const auto handles_minmax = std::minmax_element(
      process_samples.begin(), process_samples.end(),
      [](const ProcessSample& left, const ProcessSample& right) {
        return left.handles < right.handles;
      });
  const auto private_minmax = std::minmax_element(
      process_samples.begin(), process_samples.end(),
      [](const ProcessSample& left, const ProcessSample& right) {
        return left.private_bytes < right.private_bytes;
      });
  const auto pre_destroy = lifetime->snapshot();
  if (pre_destroy.maps_created != pre_destroy.maps_released ||
      pre_destroy.in_flight != 0) {
    return fail("flush did not unmap every input and release every frame lease");
  }

  const auto soak_duration = soak_seconds();
  encoder.reset();
  const auto cleanup = lifetime->snapshot();
  if (cleanup.maps_created != cleanup.maps_released ||
      cleanup.registrations_created != cleanup.registrations_released ||
      cleanup.events_created != cleanup.events_released ||
      cleanup.bitstreams_created != cleanup.bitstreams_released ||
      cleanup.sessions_created != cleanup.sessions_released || cleanup.in_flight != 0) {
    return fail("NVENC shutdown leaked a mapped/registered/event/bitstream/session resource");
  }
  if (cleanup.registrations_created == 0 ||
      cleanup.registrations_created > kPoolSize ||
      cleanup.registrations_created >= cleanup.maps_created) {
    return fail("NV12 registrations were not reused across frame submissions");
  }
  const auto post_cleanup_sample = process_sample();
  if (!post_cleanup_sample.has_value()) {
    return fail("post-cleanup process resource sampling failed");
  }

  std::string adapter_name;
  for (const wchar_t character : test_device.description.Description) {
    if (character == L'\0') {
      break;
    }
    adapter_name.push_back(character <= 0x7f ? static_cast<char>(character) : '?');
  }
  nlohmann::json evidence = {
      {"schema", 1},
      {"type", "nvenc_encode_evidence"},
      {"adapter", adapter_name},
      {"runtime_api_major", runtime_version.major},
      {"runtime_api_minor", runtime_version.minor},
      {"required_api_major", required_version.major},
      {"required_api_minor", required_version.minor},
      {"h264", capabilities.h264},
      {"nv12", capabilities.nv12},
      {"async_encode", capabilities.async_encode},
      {"max_b_frames", capabilities.max_b_frames},
      {"rate_control_modes", capabilities.rate_control_modes},
      {"max_macroblocks_per_second", capabilities.max_macroblocks_per_second},
      {"width_min", capabilities.min_width},
      {"width_max", capabilities.max_width},
      {"height_min", capabilities.min_height},
      {"height_max", capabilities.max_height},
      {"width", accepted.width},
      {"height", accepted.height},
      {"fps", accepted.fps},
      {"target_bitrate", accepted.target_bitrate},
      {"max_bitrate", accepted.max_bitrate},
      {"gop_frames", accepted.gop_frames},
      {"b_frames", accepted.frame_interval_p - 1U},
      {"lookahead_depth", accepted.lookahead_depth},
      {"spatial_aq", accepted.spatial_aq_enabled},
      {"high_profile_sps", *profile_idc == 100U},
      {"preset_p5_accepted", accepted.preset_p5},
      {"high_quality_tuning_accepted", accepted.high_quality_tuning},
      {"vbr_accepted", accepted.vbr},
      {"annex_b", true},
      {"packet_count", packet_count},
      {"idr_count", idr_count},
      {"p_picture_count", metrics.p_picture_frames},
      {"b_picture_count", metrics.b_picture_frames},
      {"maximum_idr_gap_frames", maximum_idr_gap},
      {"dts_monotonic", true},
      {"dts_initial_offset_frames", 2},
      {"pts_reorder_observed", reordered_pts_observed},
      {"driver_timestamp_packets", metrics.driver_timestamp_packets},
      {"driver_pts_digest_match",
       metrics.driver_timestamp_digest == packet_pts_digest},
      {"async_lease_retention_proved", async_lease_retention_proved},
      {"soak_seconds", soak_duration},
      {"latency_p50_us", metrics.p50_latency_us},
      {"latency_p95_us", metrics.p95_latency_us},
      {"handle_start", process_samples.front().handles},
      {"handle_peak", handles_minmax.second->handles},
      {"handle_end", process_samples.back().handles},
      {"private_bytes_start", process_samples.front().private_bytes},
      {"private_bytes_peak", private_minmax.second->private_bytes},
      {"private_bytes_end", process_samples.back().private_bytes},
      {"handle_post_cleanup", post_cleanup_sample->handles},
      {"private_bytes_post_cleanup", post_cleanup_sample->private_bytes},
      {"resource_samples", process_samples.size()},
      {"registrations_created", cleanup.registrations_created},
      {"registrations_released", cleanup.registrations_released},
      {"maps_created", cleanup.maps_created},
      {"maps_released", cleanup.maps_released},
      {"events_created", cleanup.events_created},
      {"events_released", cleanup.events_released},
      {"bitstreams_created", cleanup.bitstreams_created},
      {"bitstreams_released", cleanup.bitstreams_released},
      {"sessions_created", cleanup.sessions_created},
      {"sessions_released", cleanup.sessions_released},
      {"in_flight", cleanup.in_flight}};
  std::cout << evidence.dump() << '\n';
  return 0;
}
