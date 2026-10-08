#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/result.h"
#include "media/encoded_packet.h"
#include "media/packet_memory_budget.h"

namespace rebelliocap {

struct ReplaySnapshot {
  std::uint32_t epoch;
  QpcTicks requested_start;
  QpcTicks actual_start;
  // Requested presentation cutoff, not the maximum packet PTS. Video packets
  // form a contiguous decode prefix from the IDR through the last selected
  // picture and may include future presentation references. Muxers must retain
  // every video packet in decode order; apply presentation trimming separately
  // (e.g. container edit metadata), never filter these packets by PTS <= end.
  QpcTicks end;
  std::vector<EncodedPacket> packets;
  // Engine moves snapshots. External metadata copies need their own budget.
  std::shared_ptr<void> memory_reservation;
};

class ReplayRing {
 public:
  explicit ReplayRing(QpcTicks capacity,
      std::shared_ptr<PacketMemoryBudget> budget = std::make_shared<PacketMemoryBudget>(PacketMemoryBudget::kDefaultLimit));

  Result<void> append(EncodedPacket packet);
  Result<ReplaySnapshot> snapshot(QpcTicks end, QpcTicks duration) const;
  [[nodiscard]] std::size_t bytes() const noexcept;
  [[nodiscard]] QpcTicks retained_duration() const noexcept;
  [[nodiscard]] bool memory_limited() const noexcept;
  [[nodiscard]] std::uint64_t memory_drops() const noexcept;

 private:
  void prune_locked();
  void rebuild_keyframe_indices_locked();
  void evict_gop_locked();
  void discard_locked(const EncodedPacket& packet);

  QpcTicks capacity_;
  mutable std::mutex mutex_;
  std::deque<EncodedPacket> packets_;
  std::map<std::pair<std::uint32_t, StreamKind>, QpcTicks> latest_dts_;
  std::unordered_map<std::uint32_t, std::vector<QpcTicks>> keyframe_pts_;
  std::optional<std::uint32_t> active_epoch_;
  std::optional<QpcTicks> newest_pts_;
  std::optional<QpcTicks> newest_video_pts_;
  std::optional<QpcTicks> time_boundary_;
  bool waiting_for_video_{false};
  std::optional<QpcTicks> pruned_from_;
  std::size_t bytes_{0};
  std::size_t ring_memory_bytes_{0};
  std::shared_ptr<PacketMemoryBudget> budget_;
  bool memory_limited_{false};
  bool waiting_for_keyframe_{false};
  std::uint64_t memory_drops_{0};
  std::optional<QpcTicks> memory_boundary_;
};

}  // namespace rebelliocap
