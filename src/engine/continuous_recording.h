#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "core/result.h"
#include "media/encoded_packet.h"
#include "media/packet_memory_budget.h"
#include "media/stream_descriptor.h"
#include "mux/continuous_muxer.h"

namespace rebelliocap {
class PipelineTelemetry;

struct ContinuousQueueLimits {
  // Zero byte thresholds derive from the shared budget (hard) and 75% (soft).
  std::size_t soft_bytes{0};
  std::size_t hard_bytes{0};
  // Age enforcement is opt-in until storage A/B measurements justify defaults.
  std::chrono::milliseconds soft_age{0};
  std::chrono::milliseconds hard_age{0};
  std::size_t packet_capacity{4096};
  std::size_t batch_packets{32};
  std::size_t batch_bytes{1024 * 1024};
};

struct ContinuousQueueMetrics {
  std::size_t queued_bytes{0};
  std::size_t in_flight_bytes{0};
  std::size_t queued_packets{0};
  std::size_t in_flight_packets{0};
  QpcTicks oldest_enqueue_qpc{0};
  QpcTicks oldest_age_ticks{0};
  std::size_t high_water_bytes{0};
  bool warning{false};
  std::uint64_t warning_transitions{0};
};

class ContinuousRecording {
 public:
  static constexpr std::size_t kPacketCapacity = 4096;
  ContinuousRecording(IContinuousMuxer& muxer,
                      std::vector<StreamDescriptor> descriptors,
                      std::shared_ptr<PacketMemoryBudget> budget = std::make_shared<PacketMemoryBudget>(PacketMemoryBudget::kDefaultLimit),
                      std::function<void()> state_changed = {},
                      ContinuousQueueLimits limits = {},
                      std::shared_ptr<PipelineTelemetry> telemetry = {});
  ~ContinuousRecording();
  ContinuousRecording(const ContinuousRecording&) = delete;
  ContinuousRecording& operator=(const ContinuousRecording&) = delete;
  Result<void> start(const std::filesystem::path& destination, Container container);
  Result<void> accept(EncodedPacket packet);
  Result<void> accept_batch(std::span<const EncodedPacket> packets);
  // Host heartbeat checks age even if the disk call blocks and producers stop.
  Result<void> check_health();
  Result<void> stop();
  [[nodiscard]] ContinuousQueueMetrics queue_metrics() const;
  [[nodiscard]] bool active() const;
  [[nodiscard]] std::uint64_t packets_written() const noexcept;
  [[nodiscard]] std::uint64_t failures() const noexcept;
  [[nodiscard]] std::uint64_t packets_accepted() const noexcept;
  [[nodiscard]] std::string recovery_path() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> implementation_;
};
}  // namespace rebelliocap
