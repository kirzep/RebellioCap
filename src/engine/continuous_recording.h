#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "core/result.h"
#include "media/encoded_packet.h"
#include "media/packet_memory_budget.h"
#include "media/stream_descriptor.h"
#include "mux/continuous_muxer.h"

namespace rebelliocap {

class ContinuousRecording {
 public:
  static constexpr std::size_t kPacketCapacity = 4096;

  ContinuousRecording(IContinuousMuxer& muxer,
                      std::vector<StreamDescriptor> descriptors,
                      std::shared_ptr<PacketMemoryBudget> budget = std::make_shared<PacketMemoryBudget>(PacketMemoryBudget::kDefaultLimit),
                      std::function<void()> state_changed = {});
  ~ContinuousRecording();

  ContinuousRecording(const ContinuousRecording&) = delete;
  ContinuousRecording& operator=(const ContinuousRecording&) = delete;

  Result<void> start(const std::filesystem::path& destination,
                     Container container);
  Result<void> accept(EncodedPacket packet);
  Result<void> stop();
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
