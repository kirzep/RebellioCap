#include "video/nvenc_encoder.h"

#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform/windows/unique_handle.h"
#include "video/nvenc_api.h"
#include "video/nvenc_decode_timeline.h"
#include "video/nvenc_state_machine.h"

namespace rebelliocap {
namespace {

constexpr std::size_t kAsyncSlotCount = 6;
// A failed cancellation boundary makes this process unsafe for new sessions.
std::atomic_bool nvenc_restart_required{false};
constexpr DWORD kCompletionTimeoutMilliseconds = 10'000;
constexpr std::uint64_t kLatencyBucketWidthMicroseconds = 250;
constexpr std::size_t kLatencyBucketCount = 4096;
constexpr std::uint64_t kTimestampDigestOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kTimestampDigestPrime = 1'099'511'628'211ULL;

Error encoder_error(std::string code, std::string message,
                    std::optional<long> native = std::nullopt) {
  return Error{std::move(code), std::move(message), native};
}

bool same_com_identity(IUnknown* left, IUnknown* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  Microsoft::WRL::ComPtr<IUnknown> left_identity;
  Microsoft::WRL::ComPtr<IUnknown> right_identity;
  return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&left_identity))) &&
         SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&right_identity))) &&
         left_identity.Get() == right_identity.Get();
}

template <typename Value>
bool contains(const std::vector<Value>& values, const Value& desired) {
  return std::find(values.begin(), values.end(), desired) != values.end();
}

bool contains_guid(const std::vector<GUID>& values, const GUID& desired) {
  return std::any_of(values.begin(), values.end(), [&](const GUID& value) {
    return InlineIsEqualGUID(value, desired) != FALSE;
  });
}

std::uint64_t digest_timestamp(std::uint64_t digest, QpcTicks timestamp) {
  std::uint64_t bits = static_cast<std::uint64_t>(timestamp);
  for (std::size_t byte = 0; byte < sizeof(bits); ++byte) {
    digest ^= bits & 0xffU;
    digest *= kTimestampDigestPrime;
    bits >>= 8U;
  }
  return digest;
}

std::optional<std::uint8_t> h264_sps_profile_idc(
    const std::vector<std::byte>& bytes) {
  for (std::size_t index = 0; index + 5 < bytes.size(); ++index) {
    std::size_t nal = 0;
    const auto first = std::to_integer<unsigned char>(bytes[index]);
    const auto second = std::to_integer<unsigned char>(bytes[index + 1]);
    const auto third = std::to_integer<unsigned char>(bytes[index + 2]);
    if (first == 0 && second == 0 && third == 1) {
      nal = index + 3;
    } else if (first == 0 && second == 0 && third == 0 &&
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

Result<void> validate_config(const NvencConfig& config, QpcClock& clock) {
  if (config.width == 0 || config.height == 0 || (config.width % 2U) != 0 ||
      (config.height % 2U) != 0) {
    return Result<void>::failure(encoder_error(
        "nvenc.invalid_dimensions", "NVENC NV12 dimensions must be nonzero even values."));
  }
  if (config.fps == 0 || config.bitrate == 0 || config.gop_seconds == 0) {
    return Result<void>::failure(encoder_error(
        "nvenc.invalid_config", "NVENC FPS, bitrate, and GOP seconds must be nonzero."));
  }
  if (config.fps > (std::numeric_limits<std::uint32_t>::max)() /
                       config.gop_seconds ||
      config.bitrate > ((std::numeric_limits<std::uint32_t>::max)() / 3U) * 2U) {
    return Result<void>::failure(encoder_error(
        "nvenc.config_overflow", "NVENC GOP or maximum bitrate would overflow API fields."));
  }
  const QpcTicks frame_duration =
      clock.frequency() / static_cast<QpcTicks>(config.fps);
  if (clock.frequency() <= 0 || frame_duration <= 0 ||
      frame_duration > (std::numeric_limits<QpcTicks>::max)() / 2) {
    return Result<void>::failure(encoder_error(
        "nvenc.invalid_clock",
        "QPC cannot represent the configured video frame duration and B-frame DTS offset."));
  }
  return Result<void>::success();
}

}  // namespace

struct NvencLifetimeMetrics::State {
  std::atomic<std::uint64_t> registrations_created{0};
  std::atomic<std::uint64_t> registrations_released{0};
  std::atomic<std::uint64_t> maps_created{0};
  std::atomic<std::uint64_t> maps_released{0};
  std::atomic<std::uint64_t> events_created{0};
  std::atomic<std::uint64_t> events_released{0};
  std::atomic<std::uint64_t> bitstreams_created{0};
  std::atomic<std::uint64_t> bitstreams_released{0};
  std::atomic<std::uint64_t> sessions_created{0};
  std::atomic<std::uint64_t> sessions_released{0};
  std::atomic<std::uint64_t> in_flight{0};
};

NvencLifetimeMetrics::NvencLifetimeMetrics() : state_(std::make_unique<State>()) {}
NvencLifetimeMetrics::~NvencLifetimeMetrics() = default;

NvencLifetimeSnapshot NvencLifetimeMetrics::snapshot() const noexcept {
  return NvencLifetimeSnapshot{
      state_->registrations_created.load(), state_->registrations_released.load(),
      state_->maps_created.load(), state_->maps_released.load(),
      state_->events_created.load(), state_->events_released.load(),
      state_->bitstreams_created.load(), state_->bitstreams_released.load(),
      state_->sessions_created.load(), state_->sessions_released.load(),
      state_->in_flight.load()};
}

struct NvencEncoder::Impl {
  struct Registration {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    NV_ENC_REGISTERED_PTR registered{nullptr};
  };

  struct AsyncSlot {
    UniqueHandle event;
    bool event_registered{false};
    NV_ENC_OUTPUT_PTR bitstream{nullptr};
    NV_ENC_INPUT_PTR mapped_input{nullptr};
    Registration* registration{nullptr};
    std::optional<ConvertedVideoFrame> frame;
    QpcTicks pts{0};
    QpcTicks duration{0};
    QpcTicks submitted_at{0};
    detail::NvencTrackedSlotState lifecycle;
  };

  Impl(NvencApi loaded_api, ID3D11Device* exact_device, NvencConfig requested,
       QpcClock& qpc_clock)
      : api(std::move(loaded_api)),
        config(requested),
        clock(&qpc_clock),
        device(exact_device),
        lifetime(std::make_shared<NvencLifetimeMetrics>()) {}

  ~Impl() { shutdown(); }

  NvencApi api;
  NvencConfig config;
  QpcClock* clock;
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  void* session{nullptr};
  bool initialized{false};
  NvencCapabilities capabilities{};
  NvencAcceptedConfig accepted{};
  StreamDescriptor stream_description{};
  detail::NvencStateMachine state;
  std::shared_ptr<NvencLifetimeMetrics> lifetime;
  std::vector<std::unique_ptr<Registration>> registrations;
  std::vector<AsyncSlot> slots;
  std::deque<std::size_t> pending;
  UniqueHandle eos_event;
  bool eos_event_registered{false};
  QpcTicks last_submitted_pts{(std::numeric_limits<QpcTicks>::min)()};
  std::uint64_t submitted_frames{0};
  std::uint64_t completed_frames{0};
  std::uint64_t p_picture_frames{0};
  std::uint64_t b_picture_frames{0};
  std::uint64_t driver_timestamp_packets{0};
  std::uint64_t driver_timestamp_digest{kTimestampDigestOffset};
  detail::NvencDecodeTimeline decode_timeline;
  std::array<std::uint64_t, kLatencyBucketCount> latency_histogram{};
  std::uint64_t latency_samples{0};
  mutable std::mutex mutex;

  [[nodiscard]] const NV_ENCODE_API_FUNCTION_LIST& functions() const noexcept {
    return api.functions();
  }

  Error status_error(std::string code, std::string message,
                     NVENCSTATUS status) const {
    if (session != nullptr && functions().nvEncGetLastErrorString != nullptr) {
      const char* detail = functions().nvEncGetLastErrorString(session);
      if (detail != nullptr && detail[0] != '\0') {
        message += " Driver detail: ";
        message += detail;
      }
    }
    return encoder_error(std::move(code), std::move(message),
                         static_cast<long>(status));
  }

  Result<std::vector<GUID>> query_guids(
      PNVENCGETENCODEGUIDCOUNT count_function,
      PNVENCGETENCODEGUIDS list_function) const {
    std::uint32_t count = 0;
    NVENCSTATUS status = count_function(session, &count);
    if (status != NV_ENC_SUCCESS) {
      return Result<std::vector<GUID>>::failure(status_error(
          "nvenc.codec_query_failed", "Could not query NVENC codec count.", status));
    }
    if (count == 0) {
      return Result<std::vector<GUID>>::success({});
    }
    std::vector<GUID> values(count);
    std::uint32_t written = 0;
    status = list_function(session, values.data(), count, &written);
    if (status != NV_ENC_SUCCESS || written > count) {
      return Result<std::vector<GUID>>::failure(status_error(
          "nvenc.codec_query_failed", "Could not query NVENC codec GUIDs.", status));
    }
    values.resize(written);
    return Result<std::vector<GUID>>::success(std::move(values));
  }

  Result<std::vector<GUID>> query_profiles() const {
    std::uint32_t count = 0;
    NVENCSTATUS status = functions().nvEncGetEncodeProfileGUIDCount(
        session, NV_ENC_CODEC_H264_GUID, &count);
    if (status != NV_ENC_SUCCESS) {
      return Result<std::vector<GUID>>::failure(status_error(
          "nvenc.profile_query_failed", "Could not query H.264 profiles.", status));
    }
    if (count == 0) {
      return Result<std::vector<GUID>>::success({});
    }
    std::vector<GUID> values(count);
    std::uint32_t written = 0;
    status = functions().nvEncGetEncodeProfileGUIDs(
        session, NV_ENC_CODEC_H264_GUID, values.data(), count, &written);
    if (status != NV_ENC_SUCCESS || written > count) {
      return Result<std::vector<GUID>>::failure(status_error(
          "nvenc.profile_query_failed", "Could not enumerate H.264 profiles.", status));
    }
    values.resize(written);
    return Result<std::vector<GUID>>::success(std::move(values));
  }

  Result<std::vector<GUID>> query_presets() const {
    std::uint32_t count = 0;
    NVENCSTATUS status = functions().nvEncGetEncodePresetCount(
        session, NV_ENC_CODEC_H264_GUID, &count);
    if (status != NV_ENC_SUCCESS) {
      return Result<std::vector<GUID>>::failure(status_error(
          "nvenc.preset_query_failed", "Could not query H.264 presets.", status));
    }
    if (count == 0) {
      return Result<std::vector<GUID>>::success({});
    }
    std::vector<GUID> values(count);
    std::uint32_t written = 0;
    status = functions().nvEncGetEncodePresetGUIDs(
        session, NV_ENC_CODEC_H264_GUID, values.data(), count, &written);
    if (status != NV_ENC_SUCCESS || written > count) {
      return Result<std::vector<GUID>>::failure(status_error(
          "nvenc.preset_query_failed", "Could not enumerate H.264 presets.", status));
    }
    values.resize(written);
    return Result<std::vector<GUID>>::success(std::move(values));
  }

  Result<int> query_capability(NV_ENC_CAPS capability) const {
    NV_ENC_CAPS_PARAM parameters{};
    parameters.version = NV_ENC_CAPS_PARAM_VER;
    parameters.capsToQuery = capability;
    int value = 0;
    const NVENCSTATUS status = functions().nvEncGetEncodeCaps(
        session, NV_ENC_CODEC_H264_GUID, &parameters, &value);
    if (status != NV_ENC_SUCCESS) {
      return Result<int>::failure(status_error(
          "nvenc.capability_query_failed", "Could not query an H.264 capability.",
          status));
    }
    return Result<int>::success(value);
  }

  Result<void> inspect_capabilities() {
    auto codecs = query_guids(functions().nvEncGetEncodeGUIDCount,
                              functions().nvEncGetEncodeGUIDs);
    if (!codecs.is_success()) {
      return Result<void>::failure(std::move(codecs).error());
    }
    capabilities.h264 = contains_guid(codecs.value(), NV_ENC_CODEC_H264_GUID);
    if (!capabilities.h264) {
      return Result<void>::failure(encoder_error(
          "nvenc.h264_unavailable", "The selected NVIDIA device cannot encode H.264."));
    }

    auto profiles = query_profiles();
    if (!profiles.is_success()) {
      return Result<void>::failure(std::move(profiles).error());
    }
    if (!contains_guid(profiles.value(), NV_ENC_H264_PROFILE_HIGH_GUID)) {
      return Result<void>::failure(encoder_error(
          "nvenc.high_profile_unsupported",
          "The selected NVIDIA device does not support H.264 High profile."));
    }

    auto presets = query_presets();
    if (!presets.is_success()) {
      return Result<void>::failure(std::move(presets).error());
    }
    if (!contains_guid(presets.value(), NV_ENC_PRESET_P5_GUID)) {
      return Result<void>::failure(encoder_error(
          "nvenc.preset_p5_unsupported",
          "The selected NVIDIA device does not support NVENC preset P5."));
    }

    std::uint32_t format_count = 0;
    NVENCSTATUS status = functions().nvEncGetInputFormatCount(
        session, NV_ENC_CODEC_H264_GUID, &format_count);
    if (status != NV_ENC_SUCCESS) {
      return Result<void>::failure(status_error(
          "nvenc.input_format_query_failed", "Could not query H.264 input formats.",
          status));
    }
    if (format_count == 0) {
      return Result<void>::failure(encoder_error(
          "nvenc.nv12_unsupported", "The selected encoder exposes no H.264 input formats."));
    }
    std::vector<NV_ENC_BUFFER_FORMAT> formats(format_count);
    std::uint32_t formats_written = 0;
    status = functions().nvEncGetInputFormats(
        session, NV_ENC_CODEC_H264_GUID, formats.data(), format_count,
        &formats_written);
    if (status != NV_ENC_SUCCESS || formats_written > format_count) {
      return Result<void>::failure(status_error(
          "nvenc.input_format_query_failed", "Could not enumerate H.264 input formats.",
          status));
    }
    formats.resize(formats_written);
    capabilities.nv12 = contains(formats, NV_ENC_BUFFER_FORMAT_NV12);
    if (!capabilities.nv12) {
      return Result<void>::failure(encoder_error(
          "nvenc.nv12_unsupported", "The selected encoder does not accept NV12 input."));
    }

    const auto read = [&](NV_ENC_CAPS cap) -> Result<std::uint32_t> {
      auto value = query_capability(cap);
      if (!value.is_success()) {
        return Result<std::uint32_t>::failure(std::move(value).error());
      }
      if (value.value() < 0) {
        return Result<std::uint32_t>::failure(encoder_error(
            "nvenc.invalid_capability", "The driver returned a negative NVENC capability."));
      }
      return Result<std::uint32_t>::success(
          static_cast<std::uint32_t>(value.value()));
    };

    auto max_b = read(NV_ENC_CAPS_NUM_MAX_BFRAMES);
    auto rc = read(NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES);
    auto async = read(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT);
    auto min_width = read(NV_ENC_CAPS_WIDTH_MIN);
    auto max_width = read(NV_ENC_CAPS_WIDTH_MAX);
    auto min_height = read(NV_ENC_CAPS_HEIGHT_MIN);
    auto max_height = read(NV_ENC_CAPS_HEIGHT_MAX);
    auto mb_per_second = read(NV_ENC_CAPS_MB_PER_SEC_MAX);
    if (!max_b.is_success()) return Result<void>::failure(std::move(max_b).error());
    if (!rc.is_success()) return Result<void>::failure(std::move(rc).error());
    if (!async.is_success()) return Result<void>::failure(std::move(async).error());
    if (!min_width.is_success()) return Result<void>::failure(std::move(min_width).error());
    if (!max_width.is_success()) return Result<void>::failure(std::move(max_width).error());
    if (!min_height.is_success()) return Result<void>::failure(std::move(min_height).error());
    if (!max_height.is_success()) return Result<void>::failure(std::move(max_height).error());
    if (!mb_per_second.is_success()) {
      return Result<void>::failure(std::move(mb_per_second).error());
    }

    capabilities.max_b_frames = max_b.value();
    capabilities.rate_control_modes = rc.value();
    capabilities.async_encode = async.value() != 0;
    capabilities.min_width = min_width.value();
    capabilities.max_width = max_width.value();
    capabilities.min_height = min_height.value();
    capabilities.max_height = max_height.value();
    capabilities.max_macroblocks_per_second = mb_per_second.value();

    const std::uint64_t macroblocks_per_frame =
        static_cast<std::uint64_t>((config.width + 15U) / 16U) *
        static_cast<std::uint64_t>((config.height + 15U) / 16U);
    const std::uint64_t requested_macroblocks_per_second =
        macroblocks_per_frame * config.fps;
    if (!capabilities.async_encode || capabilities.max_b_frames < 2U ||
        (capabilities.rate_control_modes &
         static_cast<std::uint32_t>(NV_ENC_PARAMS_RC_VBR)) == 0 ||
        config.width < capabilities.min_width || config.width > capabilities.max_width ||
        config.height < capabilities.min_height ||
        config.height > capabilities.max_height ||
        requested_macroblocks_per_second >
            capabilities.max_macroblocks_per_second) {
      return Result<void>::failure(encoder_error(
          "nvenc.requested_config_unsupported",
          "NVENC rejected " + std::to_string(config.width) + "x" +
              std::to_string(config.height) + " @ " + std::to_string(config.fps) +
              " FPS. Reported capabilities: async=" +
              std::to_string(capabilities.async_encode) + ", B-frames=" +
              std::to_string(capabilities.max_b_frames) + ", rate-control-mask=" +
              std::to_string(capabilities.rate_control_modes) + ", width=" +
              std::to_string(capabilities.min_width) + ".." +
              std::to_string(capabilities.max_width) + ", height=" +
              std::to_string(capabilities.min_height) + ".." +
              std::to_string(capabilities.max_height) + ", macroblocks/sec=" +
              std::to_string(requested_macroblocks_per_second) + "/" +
              std::to_string(capabilities.max_macroblocks_per_second) +
              ". Required: async NV12 H.264 VBR with two B-frames."));
    }
    return Result<void>::success();
  }

  Result<void> initialize_encoder() {
    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    NVENCSTATUS status = functions().nvEncGetEncodePresetConfigEx(
        session, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P5_GUID,
        NV_ENC_TUNING_INFO_HIGH_QUALITY, &preset);
    if (status != NV_ENC_SUCCESS) {
      return Result<void>::failure(status_error(
          "nvenc.preset_config_failed",
          "Could not retrieve the H.264 P5 high-quality preset configuration.", status));
    }

    NV_ENC_CONFIG encoder_config = preset.presetCfg;
    encoder_config.version = NV_ENC_CONFIG_VER;
    encoder_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    encoder_config.gopLength = config.fps * config.gop_seconds;
    encoder_config.frameIntervalP = 3;
    encoder_config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    encoder_config.rcParams.version = NV_ENC_RC_PARAMS_VER;
    encoder_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    encoder_config.rcParams.averageBitRate = config.bitrate;
    encoder_config.rcParams.maxBitRate = config.bitrate + (config.bitrate / 2U);
    encoder_config.rcParams.enableLookahead = 0;
    encoder_config.rcParams.lookaheadDepth = 0;
    encoder_config.rcParams.enableAQ = 0;
    encoder_config.rcParams.aqStrength = 0;
    encoder_config.rcParams.enableTemporalAQ = 0;
    encoder_config.encodeCodecConfig.h264Config.idrPeriod = encoder_config.gopLength;
    encoder_config.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
    encoder_config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

    NV_ENC_INITIALIZE_PARAMS parameters{};
    parameters.version = NV_ENC_INITIALIZE_PARAMS_VER;
    parameters.encodeGUID = NV_ENC_CODEC_H264_GUID;
    parameters.presetGUID = NV_ENC_PRESET_P5_GUID;
    parameters.encodeWidth = config.width;
    parameters.encodeHeight = config.height;
    parameters.darWidth = config.width;
    parameters.darHeight = config.height;
    parameters.frameRateNum = config.fps;
    parameters.frameRateDen = 1;
    parameters.enableEncodeAsync = 1;
    parameters.enablePTD = 1;
    parameters.encodeConfig = &encoder_config;
    parameters.maxEncodeWidth = config.width;
    parameters.maxEncodeHeight = config.height;
    parameters.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    status = functions().nvEncInitializeEncoder(session, &parameters);
    if (status != NV_ENC_SUCCESS) {
      return Result<void>::failure(status_error(
          "nvenc.initialize_failed",
          "The NVIDIA driver rejected the fully validated H.264 baseline configuration.",
          status));
    }
    initialized = true;

    std::vector<std::byte> sequence(4096);
    std::uint32_t sequence_size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD sequence_parameters{};
    sequence_parameters.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    sequence_parameters.inBufferSize = static_cast<std::uint32_t>(sequence.size());
    sequence_parameters.spsppsBuffer = sequence.data();
    sequence_parameters.outSPSPPSPayloadSize = &sequence_size;
    status = functions().nvEncGetSequenceParams(session, &sequence_parameters);
    if (status != NV_ENC_SUCCESS || sequence_size == 0 ||
        sequence_size > sequence.size()) {
      return Result<void>::failure(status_error(
          "nvenc.sequence_query_failed",
          "The initialized encoder did not expose valid H.264 sequence parameters.",
          status));
    }
    sequence.resize(sequence_size);
    const auto profile = h264_sps_profile_idc(sequence);
    if (!profile.has_value() || *profile != 100U) {
      return Result<void>::failure(encoder_error(
          "nvenc.high_profile_not_applied",
          "The initialized H.264 SPS is not High profile (profile_idc 100)."));
    }

    stream_description = {StreamKind::Video, "h264", "Video", std::move(sequence),
                   {1, static_cast<std::int32_t>(clock->frequency())}, config.width, config.height};
    accepted.width = config.width;
    accepted.height = config.height;
    accepted.fps = config.fps;
    accepted.target_bitrate = encoder_config.rcParams.averageBitRate;
    accepted.max_bitrate = encoder_config.rcParams.maxBitRate;
    accepted.gop_frames = encoder_config.gopLength;
    accepted.frame_interval_p =
        static_cast<std::uint32_t>(encoder_config.frameIntervalP);
    accepted.lookahead_depth = encoder_config.rcParams.lookaheadDepth;
    accepted.spatial_aq_enabled = encoder_config.rcParams.enableAQ != 0;
    accepted.high_profile = true;
    accepted.preset_p5 = true;
    accepted.high_quality_tuning = true;
    accepted.vbr =
        encoder_config.rcParams.rateControlMode == NV_ENC_PARAMS_RC_VBR;
    return Result<void>::success();
  }

  Result<void> create_event(UniqueHandle& event, bool& registered) {
    event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!event) {
      return Result<void>::failure(encoder_error(
          "nvenc.event_creation_failed", "Could not create an NVENC completion event.",
          static_cast<long>(GetLastError())));
    }
    ++lifetime->state_->events_created;
    NV_ENC_EVENT_PARAMS parameters{};
    parameters.version = NV_ENC_EVENT_PARAMS_VER;
    parameters.completionEvent = event.get();
    const NVENCSTATUS status =
        functions().nvEncRegisterAsyncEvent(session, &parameters);
    if (status != NV_ENC_SUCCESS) {
      return Result<void>::failure(status_error(
          "nvenc.event_registration_failed",
          "The driver rejected an asynchronous completion event.", status));
    }
    registered = true;
    return Result<void>::success();
  }

  Result<void> create_async_resources() {
    slots.reserve(kAsyncSlotCount);
    for (std::size_t index = 0; index < kAsyncSlotCount; ++index) {
      slots.emplace_back();
      auto& slot = slots.back();
      auto event_result = create_event(slot.event, slot.event_registered);
      if (!event_result.is_success()) {
        return event_result;
      }
      NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
      bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
      const NVENCSTATUS status =
          functions().nvEncCreateBitstreamBuffer(session, &bitstream);
      if (status != NV_ENC_SUCCESS) {
        return Result<void>::failure(status_error(
            "nvenc.bitstream_creation_failed",
            "The driver could not allocate an encoded bitstream buffer.", status));
      }
      slot.bitstream = bitstream.bitstreamBuffer;
      ++lifetime->state_->bitstreams_created;
    }
    return create_event(eos_event, eos_event_registered);
  }

  Result<Registration*> registration_for(ID3D11Texture2D* texture) {
    for (const auto& registration : registrations) {
      if (registration->texture.Get() == texture) {
        return Result<Registration*>::success(registration.get());
      }
    }

    auto registration = std::make_unique<Registration>();
    registration->texture = texture;
    NV_ENC_REGISTER_RESOURCE parameters{};
    parameters.version = NV_ENC_REGISTER_RESOURCE_VER;
    parameters.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    parameters.width = config.width;
    parameters.height = config.height;
    parameters.pitch = 0;
    parameters.subResourceIndex = 0;
    parameters.resourceToRegister = texture;
    parameters.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    parameters.bufferUsage = NV_ENC_INPUT_IMAGE;
    const NVENCSTATUS status = functions().nvEncRegisterResource(session, &parameters);
    if (status != NV_ENC_SUCCESS) {
      return Result<Registration*>::failure(status_error(
          "nvenc.resource_registration_failed",
          "The driver rejected a same-device NV12 D3D11 texture registration.", status));
    }
    registration->registered = parameters.registeredResource;
    ++lifetime->state_->registrations_created;
    Registration* result = registration.get();
    registrations.push_back(std::move(registration));
    return Result<Registration*>::success(result);
  }

  Result<void> unlock_slot(AsyncSlot& slot) {
    const NVENCSTATUS status =
        functions().nvEncUnlockBitstream(session, slot.bitstream);
    if (status != NV_ENC_SUCCESS) {
      return Result<void>::failure(status_error(
          "nvenc.bitstream_unlock_failed",
          "The driver failed to unlock a completed bitstream buffer.", status));
    }
    return Result<void>::success();
  }

  Result<void> unmap_slot(AsyncSlot& slot) {
    const NVENCSTATUS status =
        functions().nvEncUnmapInputResource(session, slot.mapped_input);
    if (status != NV_ENC_SUCCESS) {
      return Result<void>::failure(status_error(
          "nvenc.resource_unmap_failed",
          "The driver failed to unmap a completed input.", status));
    }
    slot.mapped_input = nullptr;
    ++lifetime->state_->maps_released;
    return Result<void>::success();
  }

  Result<void> release_completed_slot(AsyncSlot& slot) {
    const bool was_busy = slot.lifecycle.busy;
    auto released = state.complete_slot(
        slot.lifecycle, [&] { return unlock_slot(slot); },
        [&] { return unmap_slot(slot); });
    if (!released.is_success()) {
      // The state machine deliberately leaves locked/mapped/busy flags intact
      // at the failed operation. The frame lease cannot return to Task 6's
      // pool until destructor cleanup has retried the remaining operations or
      // destroyed the encoder session.
      return released;
    }
    slot.frame.reset();
    slot.registration = nullptr;
    if (was_busy) {
      --lifetime->state_->in_flight;
    }
    return Result<void>::success();
  }

  Result<std::optional<EncodedPacket>> complete_front(DWORD wait_milliseconds) {
    if (pending.empty()) {
      return Result<std::optional<EncodedPacket>>::success(std::nullopt);
    }
    AsyncSlot& slot = slots[pending.front()];
    const DWORD wait_result = WaitForSingleObject(slot.event.get(), wait_milliseconds);
    if (wait_result == WAIT_TIMEOUT && wait_milliseconds == 0) {
      return Result<std::optional<EncodedPacket>>::success(std::nullopt);
    }
    if (wait_result == WAIT_TIMEOUT) {
      auto failed = state.fail(encoder_error(
          "nvenc.completion_timeout",
          "Timed out waiting for asynchronous NVENC completion."));
      return Result<std::optional<EncodedPacket>>::failure(
          std::move(failed).error());
    }
    if (wait_result != WAIT_OBJECT_0) {
      auto failed = state.fail(encoder_error(
          "nvenc.completion_wait_failed",
          "Waiting for an asynchronous NVENC completion event failed.",
          static_cast<long>(GetLastError())));
      return Result<std::optional<EncodedPacket>>::failure(
          std::move(failed).error());
    }
    slot.lifecycle.completion_ready = true;

    NV_ENC_LOCK_BITSTREAM locked{};
    locked.version = NV_ENC_LOCK_BITSTREAM_VER;
    locked.doNotWait = 0;
    locked.outputBitstream = slot.bitstream;
    const NVENCSTATUS lock_status =
        functions().nvEncLockBitstream(session, &locked);
    if (lock_status != NV_ENC_SUCCESS) {
      auto failed = state.fail(status_error(
          "nvenc.bitstream_lock_failed",
          "The driver failed to lock a completed bitstream buffer.", lock_status));
      auto release = release_completed_slot(slot);
      if (release.is_success()) {
        pending.pop_front();
      }
      return Result<std::optional<EncodedPacket>>::failure(
          std::move(failed).error());
    }
    slot.lifecycle.bitstream_locked = true;

    std::shared_ptr<const std::vector<std::byte>> payload;
    std::optional<Error> copy_failure;
    try {
      const auto* begin = static_cast<const std::byte*>(locked.bitstreamBufferPtr);
      if (begin == nullptr || locked.bitstreamSizeInBytes == 0) {
        copy_failure = encoder_error(
            "nvenc.empty_bitstream", "NVENC completed a frame with an empty bitstream.");
      } else {
        payload = std::make_shared<const std::vector<std::byte>>(
            begin, begin + locked.bitstreamSizeInBytes);
      }
    } catch (const std::bad_alloc&) {
      copy_failure = encoder_error(
          "nvenc.packet_allocation_failed",
          "Could not allocate storage for a completed H.264 packet.");
    }

    const bool keyframe = locked.pictureType == NV_ENC_PIC_TYPE_IDR;
    const bool duration_matches =
        locked.outputDuration == static_cast<std::uint64_t>(slot.duration);
    std::optional<QpcTicks> driver_pts;
    if (locked.outputTimeStamp <=
        static_cast<std::uint64_t>((std::numeric_limits<QpcTicks>::max)())) {
      driver_pts = static_cast<QpcTicks>(locked.outputTimeStamp);
    }
    const std::optional<QpcTicks> packet_dts = decode_timeline.take();
    const QpcTicks completed_at = clock->now();
    auto release = release_completed_slot(slot);
    if (!release.is_success()) {
      return Result<std::optional<EncodedPacket>>::failure(std::move(release).error());
    }
    pending.pop_front();
    if (copy_failure.has_value()) {
      auto failed = state.fail(std::move(*copy_failure));
      return Result<std::optional<EncodedPacket>>::failure(
          std::move(failed).error());
    }
    if (!duration_matches) {
      auto failed = state.fail(encoder_error(
          "nvenc.duration_mismatch",
          "The driver did not preserve the submitted QPC duration: submitted_duration=" +
              std::to_string(slot.duration) + ", output_duration=" +
              std::to_string(locked.outputDuration) + "."));
      return Result<std::optional<EncodedPacket>>::failure(
          std::move(failed).error());
    }
    if (!driver_pts.has_value()) {
      auto failed = state.fail(encoder_error(
          "nvenc.timestamp_out_of_range",
          "The driver returned an H.264 PTS outside signed QPC range."));
      return Result<std::optional<EncodedPacket>>::failure(
          std::move(failed).error());
    }
    if (!packet_dts.has_value() || *packet_dts > *driver_pts) {
      auto failed = state.fail(encoder_error(
          "nvenc.invalid_decode_timeline",
          "The two-B-frame decode timestamp is missing or exceeds payload PTS."));
      return Result<std::optional<EncodedPacket>>::failure(
          std::move(failed).error());
    }
    if (*packet_dts >
        (std::numeric_limits<QpcTicks>::max)() - slot.duration) {
      auto failed = state.fail(encoder_error(
          "nvenc.decode_timestamp_overflow",
          "The QPC decode timestamp timeline overflowed."));
      return Result<std::optional<EncodedPacket>>::failure(
          std::move(failed).error());
    }

    ++completed_frames;
    ++driver_timestamp_packets;
    driver_timestamp_digest =
        digest_timestamp(driver_timestamp_digest, *driver_pts);
    p_picture_frames += locked.pictureType == NV_ENC_PIC_TYPE_P ? 1U : 0U;
    b_picture_frames += locked.pictureType == NV_ENC_PIC_TYPE_B ? 1U : 0U;
    const QpcTicks latency_ticks =
        (std::max)(QpcTicks{0}, completed_at - slot.submitted_at);
    const auto latency_microseconds = static_cast<std::uint64_t>(
        (static_cast<long double>(latency_ticks) * 1'000'000.0L) /
        static_cast<long double>(clock->frequency()));
    const auto latency_bucket = (std::min)(
        static_cast<std::size_t>(latency_microseconds /
                                 kLatencyBucketWidthMicroseconds),
        kLatencyBucketCount - 1U);
    ++latency_histogram[latency_bucket];
    ++latency_samples;
    return Result<std::optional<EncodedPacket>>::success(EncodedPacket{
        StreamKind::Video, 0, *driver_pts, *packet_dts, slot.duration, keyframe,
        std::move(payload)});
  }

  Result<void> append_ready(std::vector<EncodedPacket>& packets, DWORD first_wait) {
    DWORD wait = first_wait;
    while (!pending.empty()) {
      auto completed = complete_front(wait);
      if (!completed.is_success()) {
        return Result<void>::failure(std::move(completed).error());
      }
      if (!completed.value().has_value()) {
        break;
      }
      packets.push_back(std::move(*completed.value()));
      wait = 0;
    }
    return Result<void>::success();
  }

  Result<std::vector<EncodedPacket>> encode_frame(
      ConvertedVideoFrame frame, QpcTicks pts, bool force_keyframe) {
    auto accepting = state.require_accepting();
    if (!accepting.is_success()) {
      return Result<std::vector<EncodedPacket>>::failure(
          std::move(accepting).error());
    }
    if (frame.texture() == nullptr) {
      return Result<std::vector<EncodedPacket>>::failure(encoder_error(
          "nvenc.invalid_input", "A leased NV12 D3D11 texture is required."));
    }
    if (pts <= last_submitted_pts || pts < 0) {
      return Result<std::vector<EncodedPacket>>::failure(encoder_error(
          "nvenc.non_monotonic_pts", "Video QPC timestamps must be nonnegative and increasing."));
    }

    D3D11_TEXTURE2D_DESC description{};
    frame.texture()->GetDesc(&description);
    Microsoft::WRL::ComPtr<ID3D11Device> input_device;
    frame.texture()->GetDevice(&input_device);
    if (description.Width != config.width || description.Height != config.height ||
        description.Format != DXGI_FORMAT_NV12 ||
        description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0 ||
        !same_com_identity(input_device.Get(), device.Get())) {
      return Result<std::vector<EncodedPacket>>::failure(encoder_error(
          "nvenc.invalid_input_texture",
          "Input must be a same-device GPU-only NV12 texture at configured dimensions."));
    }

    std::vector<EncodedPacket> packets;
    auto ready = append_ready(packets, 0);
    if (!ready.is_success()) {
      return Result<std::vector<EncodedPacket>>::failure(std::move(ready).error());
    }

    auto free_slot = std::find_if(slots.begin(), slots.end(),
                                  [](const AsyncSlot& slot) {
                                    return !slot.lifecycle.busy;
                                  });
    if (free_slot == slots.end()) {
      ready = append_ready(packets, kCompletionTimeoutMilliseconds);
      if (!ready.is_success()) {
        return Result<std::vector<EncodedPacket>>::failure(std::move(ready).error());
      }
      free_slot = std::find_if(slots.begin(), slots.end(),
                               [](const AsyncSlot& slot) {
                                 return !slot.lifecycle.busy;
                               });
      if (free_slot == slots.end()) {
        return Result<std::vector<EncodedPacket>>::failure(encoder_error(
            "nvenc.async_queue_stalled",
            "No asynchronous NVENC slot became available after completion."));
      }
    }
    const std::size_t slot_index =
        static_cast<std::size_t>(free_slot - slots.begin());
    AsyncSlot& slot = *free_slot;

    auto registration = registration_for(frame.texture());
    if (!registration.is_success()) {
      return Result<std::vector<EncodedPacket>>::failure(
          std::move(registration).error());
    }
    if (ResetEvent(slot.event.get()) == FALSE) {
      return Result<std::vector<EncodedPacket>>::failure(encoder_error(
          "nvenc.event_reset_failed", "Could not reset an NVENC completion event.",
          static_cast<long>(GetLastError())));
    }

    NV_ENC_MAP_INPUT_RESOURCE mapped{};
    mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapped.registeredResource = registration.value()->registered;
    NVENCSTATUS status = functions().nvEncMapInputResource(session, &mapped);
    if (status != NV_ENC_SUCCESS) {
      return Result<std::vector<EncodedPacket>>::failure(status_error(
          "nvenc.resource_map_failed", "The driver failed to map an NV12 input.",
          status));
    }
    ++lifetime->state_->maps_created;

    slot.mapped_input = mapped.mappedResource;
    slot.registration = registration.value();
    slot.frame.emplace(std::move(frame));
    slot.lifecycle.busy = true;
    slot.lifecycle.mapped = true;
    ++lifetime->state_->in_flight;

    const QpcTicks duration =
        clock->frequency() / static_cast<QpcTicks>(config.fps);
    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = config.width;
    picture.inputHeight = config.height;
    picture.inputPitch = config.width;
    picture.encodePicFlags = force_keyframe
                                 ? (NV_ENC_PIC_FLAG_FORCEIDR |
                                    NV_ENC_PIC_FLAG_OUTPUT_SPSPPS)
                                 : 0U;
    picture.frameIdx = static_cast<std::uint32_t>(submitted_frames);
    picture.inputTimeStamp = static_cast<std::uint64_t>(pts);
    picture.inputDuration = static_cast<std::uint64_t>(duration);
    picture.inputBuffer = mapped.mappedResource;
    picture.outputBitstream = slot.bitstream;
    picture.completionEvent = slot.event.get();
    picture.bufferFmt = mapped.mappedBufferFmt;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    const QpcTicks submitted_at = clock->now();
    status = functions().nvEncEncodePicture(session, &picture);
    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
      auto failed = state.fail(status_error(
          "nvenc.encode_submission_failed",
          "The driver rejected an asynchronous H.264 frame submission.", status));
      slot.lifecycle.completion_ready = true;
      static_cast<void>(release_completed_slot(slot));
      return Result<std::vector<EncodedPacket>>::failure(
          std::move(failed).error());
    }

    slot.pts = pts;
    slot.duration = duration;
    slot.submitted_at = submitted_at;
    pending.push_back(slot_index);
    decode_timeline.submit(pts, duration);
    last_submitted_pts = pts;
    ++submitted_frames;

    ready = append_ready(packets, 0);
    if (!ready.is_success()) {
      return Result<std::vector<EncodedPacket>>::failure(std::move(ready).error());
    }
    return Result<std::vector<EncodedPacket>>::success(std::move(packets));
  }

  Result<std::vector<EncodedPacket>> flush_encoder() {
    std::vector<EncodedPacket> packets;
    auto flushed = state.flush(
        [&]() -> Result<void> {
          if (ResetEvent(eos_event.get()) == FALSE) {
            return Result<void>::failure(encoder_error(
                "nvenc.eos_event_reset_failed",
                "Could not reset the NVENC EOS event.",
                static_cast<long>(GetLastError())));
          }
          NV_ENC_PIC_PARAMS eos{};
          eos.version = NV_ENC_PIC_PARAMS_VER;
          eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
          eos.completionEvent = eos_event.get();
          const NVENCSTATUS status = functions().nvEncEncodePicture(session, &eos);
          if (status != NV_ENC_SUCCESS) {
            return Result<void>::failure(status_error(
                "nvenc.eos_submission_failed",
                "The driver rejected deterministic NVENC EOS.", status));
          }
          return Result<void>::success();
        },
        [&]() -> Result<void> {
          while (!pending.empty()) {
            auto ready = append_ready(packets, kCompletionTimeoutMilliseconds);
            if (!ready.is_success()) {
              return ready;
            }
          }
          return Result<void>::success();
        },
        [&]() -> Result<void> {
          const DWORD eos_wait =
              WaitForSingleObject(eos_event.get(), kCompletionTimeoutMilliseconds);
          if (eos_wait != WAIT_OBJECT_0) {
            return Result<void>::failure(encoder_error(
                eos_wait == WAIT_TIMEOUT ? "nvenc.eos_timeout"
                                         : "nvenc.eos_wait_failed",
                "NVENC did not complete deterministic EOS within the timeout.",
                eos_wait == WAIT_FAILED
                    ? std::optional<long>(GetLastError())
                    : std::nullopt));
          }
          return Result<void>::success();
        });
    if (!flushed.is_success()) {
      return Result<std::vector<EncodedPacket>>::failure(
          std::move(flushed).error());
    }
    return Result<std::vector<EncodedPacket>>::success(std::move(packets));
  }

  void close_event(UniqueHandle& event, bool& registered) noexcept {
    bool registration_released = !registered;
    if (session != nullptr && registered) {
      NV_ENC_EVENT_PARAMS parameters{};
      parameters.version = NV_ENC_EVENT_PARAMS_VER;
      parameters.completionEvent = event.get();
      registration_released =
          functions().nvEncUnregisterAsyncEvent(session, &parameters) ==
          NV_ENC_SUCCESS;
      registered = false;
    }
    if (event) {
      event.reset();
      if (registration_released) {
        ++lifetime->state_->events_released;
      }
    }
  }

  void shutdown() noexcept {
    if (session == nullptr) {
      return;
    }
    if (initialized &&
        (state.state() == detail::NvencEncoderState::Accepting ||
         state.state() == detail::NvencEncoderState::EosSubmitted)) {
      try {
        static_cast<void>(flush_encoder());
      } catch (...) {
      }
    }

    for (auto& slot : slots) {
      if (!slot.lifecycle.completion_ready) {
        continue;
      }
      const bool was_busy = slot.lifecycle.busy;
      try {
        auto cleaned = state.cleanup_slot(
            slot.lifecycle, [&] { return unlock_slot(slot); },
            [&] { return unmap_slot(slot); });
        if (cleaned.is_success()) {
          slot.frame.reset();
          slot.registration = nullptr;
          if (was_busy) {
            --lifetime->state_->in_flight;
          }
        }
      } catch (...) {
      }
    }

    const bool unresolved_slots = std::any_of(
        slots.begin(), slots.end(), [](const AsyncSlot& slot) {
          return detail::NvencStateMachine::slot_is_active(slot.lifecycle);
        });
    if (unresolved_slots) {
      // A wait/unlock/unmap failure may leave driver-owned work in an unknown
      // state. Do not unmap, unregister, or destroy dependent resources while
      // that work may still reference them. Session destruction is the safe
      // final cancellation boundary; leases are released only afterward.
      const bool session_destroyed =
          functions().nvEncDestroyEncoder(session) == NV_ENC_SUCCESS;
      if (!session_destroyed) {
        nvenc_restart_required.store(true);
        OutputDebugStringW(L"RebellioCap: NVENC cancellation failed; owners retained. Engine process restart required.\n");
        return;
      }
      if (session_destroyed) {
        ++lifetime->state_->sessions_released;
      }
      session = nullptr;

      for (auto& slot : slots) {
        if (session_destroyed && slot.lifecycle.mapped) {
          ++lifetime->state_->maps_released;
        }
        slot.mapped_input = nullptr;
        slot.lifecycle.mapped = false;
        slot.lifecycle.bitstream_locked = false;
        slot.lifecycle.completion_ready = false;
        if (slot.lifecycle.busy) {
          slot.lifecycle.busy = false;
          --lifetime->state_->in_flight;
        }
        slot.frame.reset();
        slot.registration = nullptr;
        if (slot.bitstream != nullptr) {
          if (session_destroyed) {
            ++lifetime->state_->bitstreams_released;
          }
          slot.bitstream = nullptr;
        }
        if (slot.event) {
          slot.event.reset();
          if (session_destroyed || !slot.event_registered) {
            ++lifetime->state_->events_released;
          }
        }
        slot.event_registered = false;
      }
      if (eos_event) {
        eos_event.reset();
        if (session_destroyed || !eos_event_registered) {
          ++lifetime->state_->events_released;
        }
      }
      eos_event_registered = false;
      for (auto& registration : registrations) {
        if (session_destroyed && registration->registered != nullptr) {
          ++lifetime->state_->registrations_released;
        }
        registration->registered = nullptr;
      }
      registrations.clear();
      pending.clear();
      return;
    }
    pending.clear();

    for (auto& registration : registrations) {
      if (registration->registered != nullptr) {
        if (functions().nvEncUnregisterResource(session, registration->registered) ==
            NV_ENC_SUCCESS) {
          ++lifetime->state_->registrations_released;
        }
        registration->registered = nullptr;
      }
    }
    registrations.clear();

    for (auto& slot : slots) {
      if (slot.bitstream != nullptr) {
        if (functions().nvEncDestroyBitstreamBuffer(session, slot.bitstream) ==
            NV_ENC_SUCCESS) {
          ++lifetime->state_->bitstreams_released;
        }
        slot.bitstream = nullptr;
      }
      close_event(slot.event, slot.event_registered);
    }
    close_event(eos_event, eos_event_registered);
    if (functions().nvEncDestroyEncoder(session) == NV_ENC_SUCCESS) {
      ++lifetime->state_->sessions_released;
    }
    session = nullptr;
  }
};

NvencEncoder::NvencEncoder(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

NvencEncoder::~NvencEncoder() {
  implementation_->shutdown();
  if (implementation_->session != nullptr) {
    // No completion/cancellation boundary exists. Deliberately retain the whole
    // ownership graph until OS process teardown, including the loaded API DLL,
    // device, registered textures, events, bitstreams and converter pool leases.
    // Do not register a static destructor or retry an unknown driver state.
    static_cast<void>(implementation_.release());
  }
}

Result<std::unique_ptr<NvencEncoder>> NvencEncoder::create(
    ID3D11Device* device, NvencConfig config, QpcClock& clock) {
  if (nvenc_restart_required.load()) {
    return Result<std::unique_ptr<NvencEncoder>>::failure(encoder_error(
        "nvenc.restart_required",
        "NVENC cancellation failed; retained resources require an engine process restart."));
  }
  auto validation = validate_config(config, clock);
  if (!validation.is_success()) {
    return Result<std::unique_ptr<NvencEncoder>>::failure(
        std::move(validation).error());
  }
  if (device == nullptr) {
    return Result<std::unique_ptr<NvencEncoder>>::failure(encoder_error(
        "nvenc.invalid_device", "An exact D3D11 device is required."));
  }

  auto api_result = NvencApi::load();
  if (!api_result.is_success()) {
    return Result<std::unique_ptr<NvencEncoder>>::failure(
        std::move(api_result).error());
  }
  auto implementation = std::make_unique<Impl>(
      std::move(api_result).value(), device, config, clock);

  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
  open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
  open.device = device;
  open.apiVersion = NVENCAPI_VERSION;
  const NVENCSTATUS open_status = implementation->functions().nvEncOpenEncodeSessionEx(
      &open, &implementation->session);
  if (open_status != NV_ENC_SUCCESS) {
    const std::string code =
        (open_status == NV_ENC_ERR_NO_ENCODE_DEVICE)
            ? "nvenc.no_encode_device"
            : (open_status == NV_ENC_ERR_UNSUPPORTED_DEVICE
                   ? "nvenc.unsupported_device"
                   : "nvenc.session_open_failed");
    return Result<std::unique_ptr<NvencEncoder>>::failure(
        implementation->status_error(
            code, "Could not open a direct NVENC session on the exact D3D11 device.",
            open_status));
  }
  ++implementation->lifetime->state_->sessions_created;

  auto capabilities = implementation->inspect_capabilities();
  if (!capabilities.is_success()) {
    return Result<std::unique_ptr<NvencEncoder>>::failure(
        std::move(capabilities).error());
  }
  auto initialized = implementation->initialize_encoder();
  if (!initialized.is_success()) {
    return Result<std::unique_ptr<NvencEncoder>>::failure(
        std::move(initialized).error());
  }
  auto resources = implementation->create_async_resources();
  if (!resources.is_success()) {
    return Result<std::unique_ptr<NvencEncoder>>::failure(
        std::move(resources).error());
  }

  return Result<std::unique_ptr<NvencEncoder>>::success(
      std::unique_ptr<NvencEncoder>(new NvencEncoder(std::move(implementation))));
}

Result<std::vector<EncodedPacket>> NvencEncoder::encode(
    ConvertedVideoFrame frame, QpcTicks pts, bool force_keyframe) {
  const std::lock_guard lock(implementation_->mutex);
  return implementation_->encode_frame(std::move(frame), pts, force_keyframe);
}

Result<std::vector<EncodedPacket>> NvencEncoder::flush() {
  const std::lock_guard lock(implementation_->mutex);
  return implementation_->flush_encoder();
}

const NvencCapabilities& NvencEncoder::capabilities() const noexcept {
  return implementation_->capabilities;
}

const StreamDescriptor& NvencEncoder::descriptor() const noexcept {
  return implementation_->stream_description;
}

const NvencAcceptedConfig& NvencEncoder::accepted_config() const noexcept {
  return implementation_->accepted;
}

NvencRuntimeMetrics NvencEncoder::metrics() const {
  const std::lock_guard lock(implementation_->mutex);
  NvencRuntimeMetrics result;
  result.submitted_frames = implementation_->submitted_frames;
  result.completed_frames = implementation_->completed_frames;
  result.in_flight_frames = static_cast<std::uint64_t>(std::count_if(
      implementation_->slots.begin(), implementation_->slots.end(),
      [](const Impl::AsyncSlot& slot) {
        return detail::NvencStateMachine::slot_is_active(slot.lifecycle);
      }));
  result.p_picture_frames = implementation_->p_picture_frames;
  result.b_picture_frames = implementation_->b_picture_frames;
  result.driver_timestamp_packets =
      implementation_->driver_timestamp_packets;
  result.driver_timestamp_digest = implementation_->driver_timestamp_digest;
  if (implementation_->latency_samples == 0) {
    return result;
  }
  const auto percentile = [&](std::size_t numerator) {
    const std::uint64_t desired =
        (implementation_->latency_samples * numerator + 99U) / 100U;
    std::uint64_t cumulative = 0;
    for (std::size_t index = 0; index < kLatencyBucketCount; ++index) {
      cumulative += implementation_->latency_histogram[index];
      if (cumulative >= desired) {
        return static_cast<std::uint64_t>(index + 1U) *
               kLatencyBucketWidthMicroseconds;
      }
    }
    return static_cast<std::uint64_t>(kLatencyBucketCount) *
           kLatencyBucketWidthMicroseconds;
  };
  result.p50_latency_us = percentile(50);
  result.p95_latency_us = percentile(95);
  return result;
}

std::shared_ptr<const NvencLifetimeMetrics> NvencEncoder::lifetime_metrics()
    const noexcept {
  return implementation_->lifetime;
}

}  // namespace rebelliocap
