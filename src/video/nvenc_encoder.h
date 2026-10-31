#pragma once

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <optional>

#include "core/qpc_clock.h"
#include "core/result.h"
#include "capture/gpu_recording_priority.h"
#include "video/video_encoder.h"
#include "media/stream_descriptor.h"

namespace rebelliocap {
class PipelineTelemetry;

struct NvencConfig {
  std::uint32_t width{1920};
  std::uint32_t height{1080};
  std::uint32_t fps{60};
  std::uint32_t bitrate{30'000'000};
  std::uint32_t gop_seconds{2};
  std::uint32_t slot_count{6};
  std::shared_ptr<PipelineTelemetry> telemetry;
  std::uint32_t preset{5};
  std::uint32_t b_frames{2};
  // nullopt preserves the driver's preset baseline; 0 explicitly disables it.
  std::optional<std::uint32_t> multipass;
  bool constant_qp{false};
  std::uint32_t qp_i{20}, qp_p{23}, qp_b{25};
  std::uint32_t target_quality{0};
  std::uint32_t completion_stall_timeout_ms{30'000};
};

struct NvencCapabilities {
  bool h264{false};
  bool nv12{false};
  bool async_encode{false};
  std::uint32_t max_b_frames{0};
  std::uint32_t rate_control_modes{0};
  std::uint32_t min_width{0};
  std::uint32_t max_width{0};
  std::uint32_t min_height{0};
  std::uint32_t max_height{0};
  std::uint32_t max_macroblocks_per_second{0};
};

// These are the exact values accepted by nvEncInitializeEncoder. They are not
// queried back after initialization; stream behavior is verified from output.
struct NvencAcceptedConfig {
  bool constant_qp{false};
  std::uint32_t qp_i{0}, qp_p{0}, qp_b{0};
  std::uint32_t preset_number{0};
  std::uint32_t multipass{0};
  std::uint32_t rate_control{0};
  std::uint32_t target_quality{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t fps{0};
  std::uint32_t target_bitrate{0};
  std::uint32_t max_bitrate{0};
  std::uint32_t gop_frames{0};
  std::uint32_t frame_interval_p{0};
  std::uint32_t lookahead_depth{0};
  bool spatial_aq_enabled{false};
  bool high_profile{false};
  bool preset_p5{false};
  bool high_quality_tuning{false};
  bool vbr{false};
};

struct CaptureSessionDiagnostics {
  bool available{false};
  bool game_window{false};
  bool duplicate_output1{false};
  bool texture_format_available{false};
  std::uint32_t texture_format{0};
  std::uint32_t adapter_luid_low{0};
  std::int32_t adapter_luid_high{0};
  std::optional<long> fallback_hresult;
};

struct NvencRuntimeMetrics {
  bool recording_priority_requested{false}, recording_priority_applied{false};
  int recording_gpu_priority{0};
  bool recording_gpu_priority_available{true};
  GpuRecordingPriorityMode recording_priority_mode{GpuRecordingPriorityMode::Disabled};
  std::optional<long> recording_priority_hresult;
  CaptureSessionDiagnostics capture_session;
  bool capture_waiting_for_game{false};
  std::uint64_t encoder_queue_depth{0}, encoder_queue_capacity{0}, encoder_queue_dropped_frames{0};
  NvencAcceptedConfig accepted_config;
  std::uint64_t new_image_ticks{0}, repeated_image_ticks{0}, no_image_ticks{0};
  std::uint64_t converter_cache_hits{0}, converter_pool_capacity{0};
  std::uint64_t converter_input_views_created{0};
  bool direct_nv12_cache{false};
  std::uint64_t submitted_frames{0};
  std::uint64_t completed_frames{0};
  std::uint64_t backpressure_dropped_frames{0};
  std::uint64_t oldest_in_flight_age_us{0};
  std::uint64_t in_flight_frames{0};
  std::uint64_t slot_capacity{0};
  std::uint64_t max_in_flight_frames{0};
  std::uint64_t p50_latency_us{0};
  std::uint64_t p95_latency_us{0};
  std::uint64_t p_picture_frames{0};
  std::uint64_t b_picture_frames{0};
  std::uint64_t driver_timestamp_packets{0};
  std::uint64_t driver_timestamp_digest{0};
};

struct EncoderAdmission {
  std::vector<EncodedPacket> completed;
  bool can_submit{false};
};

struct NvencLifetimeSnapshot {
  std::uint64_t registrations_created{0};
  std::uint64_t registrations_released{0};
  std::uint64_t maps_created{0};
  std::uint64_t maps_released{0};
  std::uint64_t events_created{0};
  std::uint64_t events_released{0};
  std::uint64_t bitstreams_created{0};
  std::uint64_t bitstreams_released{0};
  std::uint64_t sessions_created{0};
  std::uint64_t sessions_released{0};
  std::uint64_t in_flight{0};
};

class NvencLifetimeMetrics {
 public:
  NvencLifetimeMetrics();
  ~NvencLifetimeMetrics();

  [[nodiscard]] NvencLifetimeSnapshot snapshot() const noexcept;

 private:
  friend struct NvencShutdownTestAccess;
  struct State;
  friend class NvencEncoder;

  std::unique_ptr<State> state_;
};

class NvencEncoder final : public IVideoEncoder {
 public:
  static Result<std::unique_ptr<NvencEncoder>> create(
      ID3D11Device* device, NvencConfig config, QpcClock& clock);

  ~NvencEncoder() override;
  NvencEncoder(const NvencEncoder&) = delete;
  NvencEncoder& operator=(const NvencEncoder&) = delete;

  Result<std::vector<EncodedPacket>> encode(
      ConvertedVideoFrame frame, QpcTicks pts, bool force_keyframe) override;
  // Capture-thread only. Collect completions before leasing a converter output.
  Result<EncoderAdmission> prepare_frame(bool force_keyframe);
  Result<std::vector<EncodedPacket>> flush() override;

  [[nodiscard]] const StreamDescriptor& descriptor() const noexcept;
  [[nodiscard]] const NvencCapabilities& capabilities() const noexcept;
  [[nodiscard]] const NvencAcceptedConfig& accepted_config() const noexcept;
  [[nodiscard]] NvencRuntimeMetrics metrics() const;
  [[nodiscard]] std::shared_ptr<const NvencLifetimeMetrics> lifetime_metrics()
      const noexcept;

 private:
  friend struct NvencShutdownTestAccess;
  struct Impl;

  explicit NvencEncoder(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> implementation_;
};

}  // namespace rebelliocap
