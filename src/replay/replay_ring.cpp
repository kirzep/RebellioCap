#include "replay/replay_ring.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace rebelliocap {
namespace {

Error replay_error(std::string code, std::string message) {
  return {.code = std::move(code), .message = std::move(message), .hresult = std::nullopt};
}

QpcTicks saturating_subtract(QpcTicks value, QpcTicks amount) {
  if (amount > 0 && value < std::numeric_limits<QpcTicks>::min() + amount) {
    return std::numeric_limits<QpcTicks>::min();
  }
  return value - amount;
}

QpcTicks saturating_duration(QpcTicks latest, QpcTicks earliest) {
  if (earliest < 0 && latest > std::numeric_limits<QpcTicks>::max() + earliest) {
    return std::numeric_limits<QpcTicks>::max();
  }
  return latest - earliest;
}

std::size_t payload_bytes(const EncodedPacket& packet) {
  return packet.payload == nullptr ? 0 : packet.payload->size();
}

}  // namespace

ReplayRing::ReplayRing(QpcTicks capacity, std::shared_ptr<PacketMemoryBudget> budget)
    : capacity_(std::max<QpcTicks>(capacity, 0)), budget_(std::move(budget)) {}

Result<void> ReplayRing::append(EncodedPacket packet) {
  std::scoped_lock lock(mutex_);

  const auto stream = std::make_pair(packet.epoch, packet.stream);
  const auto latest = latest_dts_.find(stream);
  if (latest != latest_dts_.end() && packet.dts <= latest->second) {
    return Result<void>::failure(replay_error(
        "replay.non_monotonic_packet",
        "Packet decode timestamps must be strictly increasing within a stream."));
  }

  latest_dts_[stream] = packet.dts;
  if (active_epoch_ != packet.epoch) {
    packets_.clear();
    keyframe_pts_.clear();
    bytes_ = 0;
    ring_memory_bytes_ = 0;
    waiting_for_keyframe_ = false;
    memory_boundary_.reset();
    newest_pts_.reset();
    newest_video_pts_.reset();
    time_boundary_.reset();
    waiting_for_video_ = false;
    pruned_from_.reset();
  }
  active_epoch_ = packet.epoch;
  newest_pts_ = newest_pts_.has_value() ? std::max(*newest_pts_, packet.pts) : packet.pts;
  const auto cutoff = saturating_subtract(*newest_pts_, capacity_);
  // GOP-aligned pruning cannot advance while capture produces no video.
  // Once every video picture has expired, keeping its old IDR would pin an
  // unlimited audio tail. Discard the unusable window and wait for a fresh IDR.
  if (!packets_.empty() && newest_video_pts_.value_or(packets_.front().pts) < cutoff) {
    packets_.clear();
    keyframe_pts_.clear();
    bytes_ = 0;
    ring_memory_bytes_ = 0;
    newest_video_pts_.reset();
    pruned_from_.reset();
    time_boundary_ = cutoff;
    waiting_for_video_ = true;
  }
  if (waiting_for_video_) {
    if (packet.stream != StreamKind::Video || !packet.keyframe || packet.pts < cutoff)
      return Result<void>::success();
    waiting_for_video_ = false;
    time_boundary_ = packet.pts;
  }
  if (time_boundary_.has_value() && packet.pts < *time_boundary_)
    return Result<void>::success();
  if ((waiting_for_keyframe_ && !(packet.stream == StreamKind::Video && packet.keyframe)) ||
      (memory_boundary_.has_value() && packet.pts < *memory_boundary_)) {
    ++memory_drops_;
    return Result<void>::success();
  }
  auto tracked = budget_->track(packet.payload);
  while (!tracked.is_success() && !packets_.empty()) {
    evict_gop_locked();
    tracked = budget_->track(packet.payload);
  }
  if (!tracked.is_success()) {
    memory_limited_ = true;
    waiting_for_keyframe_ = true;
    ++memory_drops_;
    return Result<void>::failure(tracked.error());
  }
  packet.payload = std::move(tracked).value();
  if (waiting_for_keyframe_) {
    if (packet.stream != StreamKind::Video || !packet.keyframe) {
      ++memory_drops_;
      return Result<void>::success();
    }
    waiting_for_keyframe_ = false;
    memory_boundary_ = packet.pts;
  }
  bytes_ += payload_bytes(packet);
  ring_memory_bytes_ += PacketMemoryBudget::payload_cost(packet.payload);
  packets_.push_back(std::move(packet));
  if (packets_.back().stream == StreamKind::Video)
    newest_video_pts_ = newest_video_pts_.has_value()
        ? std::max(*newest_video_pts_, packets_.back().pts) : packets_.back().pts;
  if (packets_.back().stream == StreamKind::Video && packets_.back().keyframe) {
    keyframe_pts_[packets_.back().epoch].push_back(packets_.back().pts);
  }
  prune_locked();
  // Leave a quarter of the shared budget for pending saves and the continuous
  // recording queue. A snapshot can keep payloads alive after ring eviction.
  const auto ring_target = budget_->limit() - budget_->limit() / 4;
  while (ring_memory_bytes_ > ring_target && !packets_.empty()) evict_gop_locked();
  if (memory_limited_ && !waiting_for_keyframe_ && memory_boundary_.has_value() &&
      !packets_.empty() && packets_.back().stream == StreamKind::Video &&
      saturating_duration(packets_.back().pts, *memory_boundary_) >= capacity_) {
    memory_limited_ = false;
  }
  return Result<void>::success();
}

Result<ReplaySnapshot> ReplayRing::snapshot(QpcTicks end, QpcTicks duration) const {
  if (duration < 0) {
    return Result<ReplaySnapshot>::failure(
        replay_error("replay.invalid_duration", "Snapshot duration cannot be negative."));
  }

  std::scoped_lock lock(mutex_);
  if (waiting_for_video_) {
    return Result<ReplaySnapshot>::failure(replay_error("replay.keyframe_unavailable",
        "Replay expired while video capture was unavailable; wait for a fresh video keyframe."));
  }
  const QpcTicks requested_start = saturating_subtract(end, duration);
  if (waiting_for_keyframe_) {
    return Result<ReplaySnapshot>::failure(replay_error(
        "replay.memory_budget_exceeded", "Replay is waiting for a keyframe after reaching its memory budget. Wait for pending saves or reduce Replay duration."));
  }

  std::optional<std::uint32_t> epoch;
  for (auto packet = packets_.rbegin(); packet != packets_.rend(); ++packet) {
    if (packet->stream == StreamKind::Video && packet->pts <= end) {
      epoch = packet->epoch;
      break;
    }
  }
  if (!epoch.has_value()) {
    return Result<ReplaySnapshot>::failure(
        replay_error("replay.keyframe_unavailable", "No video packet is available at the requested end."));
  }

  const auto keyframes = keyframe_pts_.find(*epoch);
  if (keyframes == keyframe_pts_.end()) {
    return Result<ReplaySnapshot>::failure(
        replay_error("replay.keyframe_unavailable", "No video keyframe is available for the requested range."));
  }

  std::optional<QpcTicks> actual_start;
  for (auto keyframe = keyframes->second.rbegin(); keyframe != keyframes->second.rend(); ++keyframe) {
    if (*keyframe <= requested_start) {
      actual_start = *keyframe;
      break;
    }
  }
  // A newly started Replay buffer can be shorter than the requested clip.
  // Save the available footage from its earliest decodable frame.
  if (!actual_start.has_value()) {
    for (const auto keyframe : keyframes->second) {
      if (keyframe <= end &&
          (!actual_start.has_value() || keyframe < *actual_start)) {
        actual_start = keyframe;
      }
    }
  }
  if (!actual_start.has_value()) {
    return Result<ReplaySnapshot>::failure(
        replay_error("replay.keyframe_unavailable", "No video keyframe precedes the requested range."));
  }

  ReplaySnapshot result{.epoch = *epoch,
                        .requested_start = std::max(requested_start, *actual_start),
                        .actual_start = *actual_start,
                        .end = end,
                        .packets = {}};
  auto first_video = packets_.end();
  auto last_video = packets_.end();
  for (auto packet = packets_.begin(); packet != packets_.end(); ++packet) {
    if (packet->epoch != *epoch || packet->stream != StreamKind::Video) {
      continue;
    }
    if (first_video == packets_.end() && packet->keyframe &&
        packet->pts == *actual_start) {
      first_video = packet;
    }
    if (first_video != packets_.end() && packet->pts >= *actual_start &&
        packet->pts <= end) {
      last_video = packet;
    }
  }
  const auto selected_packet = [&](auto item) {
    const auto& packet = *item;
    return packet.epoch == *epoch && (packet.stream == StreamKind::Video
        ? first_video != packets_.end() && last_video != packets_.end() &&
              item >= first_video && item <= last_video
        : packet.pts >= *actual_start && packet.pts <= end);
  };
  std::size_t count = 0;
  for (auto item = packets_.begin(); item != packets_.end(); ++item) {
    if (selected_packet(item)) ++count;
  }
  if (count > (std::numeric_limits<std::size_t>::max)() / sizeof(EncodedPacket)) {
    return Result<ReplaySnapshot>::failure(replay_error("replay.memory_budget_exceeded", "Replay snapshot metadata exceeds the memory budget."));
  }
  auto reservation = budget_->reserve(count * sizeof(EncodedPacket));
  if (!reservation.is_success()) return Result<ReplaySnapshot>::failure(reservation.error());
  result.memory_reservation = std::move(reservation).value();
  result.packets.reserve(count);
  for (auto item = packets_.begin(); item != packets_.end(); ++item) {
    const auto& packet = *item;
    const bool selected = packet.stream == StreamKind::Video
        ? first_video != packets_.end() && last_video != packets_.end() &&
              item >= first_video && item <= last_video
        : packet.pts >= *actual_start && packet.pts <= end;
    if (packet.epoch == *epoch && selected) {
      result.packets.push_back(packet);
    }
  }
  return Result<ReplaySnapshot>::success(std::move(result));
}

std::size_t ReplayRing::bytes() const noexcept {
  std::scoped_lock lock(mutex_);
  return bytes_;
}

QpcTicks ReplayRing::retained_duration() const noexcept {
  std::scoped_lock lock(mutex_);
  if (!active_epoch_.has_value()) {
    return 0;
  }

  std::optional<QpcTicks> earliest;
  std::optional<QpcTicks> latest;
  for (const auto& packet : packets_) {
    if (packet.epoch != *active_epoch_) {
      continue;
    }
    earliest = earliest.has_value() ? std::min(*earliest, packet.pts) : packet.pts;
    latest = latest.has_value() ? std::max(*latest, packet.pts) : packet.pts;
  }
  if (!earliest.has_value() || !latest.has_value()) {
    return 0;
  }
  return saturating_duration(*latest, *earliest);
}

bool ReplayRing::memory_limited() const noexcept {
  std::scoped_lock lock(mutex_);
  return memory_limited_;
}

std::uint64_t ReplayRing::memory_drops() const noexcept {
  std::scoped_lock lock(mutex_);
  return memory_drops_;
}

void ReplayRing::discard_locked(const EncodedPacket& packet) {
  bytes_ -= payload_bytes(packet);
  ring_memory_bytes_ -= PacketMemoryBudget::payload_cost(packet.payload);
}

void ReplayRing::evict_gop_locked() {
  memory_limited_ = true;
  bool seen_keyframe = false;
  auto boundary = packets_.end();
  for (auto item = packets_.begin(); item != packets_.end(); ++item) {
    if (item->stream == StreamKind::Video && item->keyframe) {
      if (seen_keyframe) { boundary = item; break; }
      seen_keyframe = true;
    }
  }
  if (boundary == packets_.end()) {
    memory_drops_ += packets_.size();
    packets_.clear();
    keyframe_pts_.clear();
    bytes_ = 0;
    ring_memory_bytes_ = 0;
    pruned_from_.reset();
    memory_boundary_.reset();
    waiting_for_keyframe_ = true;
    return;
  }
  const auto offset = static_cast<std::size_t>(boundary - packets_.begin());
  const auto pts = boundary->pts;
  std::size_t index = 0;
  const auto remove = std::remove_if(packets_.begin(), packets_.end(), [&](const auto& packet) {
    const bool discard = packet.stream == StreamKind::Video ? index < offset : packet.pts < pts;
    ++index;
    if (discard) { discard_locked(packet); ++memory_drops_; }
    return discard;
  });
  packets_.erase(remove, packets_.end());
  memory_boundary_ = pts;
  pruned_from_.reset();
  rebuild_keyframe_indices_locked();
}

void ReplayRing::prune_locked() {
  if (!active_epoch_.has_value() || !newest_pts_.has_value()) {
    return;
  }

  const QpcTicks cutoff = saturating_subtract(*newest_pts_, capacity_);
  std::optional<QpcTicks> keep_from;
  const auto keyframes = keyframe_pts_.find(*active_epoch_);
  if (keyframes != keyframe_pts_.end()) {
    for (auto keyframe = keyframes->second.rbegin(); keyframe != keyframes->second.rend(); ++keyframe) {
      if (*keyframe <= cutoff) {
        keep_from = *keyframe;
        break;
      }
    }
  }

  if (!keep_from.has_value()) {
    return;
  }
  if (keep_from == pruned_from_) {
    // Existing packets already satisfy this boundary. A delayed audio or
    // reordered video packet can still arrive behind it and must be discarded.
    if (packets_.back().pts < *keep_from) {
      discard_locked(packets_.back());
      packets_.pop_back();
    }
    return;
  }
  pruned_from_ = keep_from;
  if (memory_boundary_.has_value() && *keep_from > *memory_boundary_) {
    memory_boundary_ = keep_from;
  }

  const auto remove_begin = std::remove_if(
      packets_.begin(), packets_.end(), [this, keep_from](const EncodedPacket& packet) {
        const bool remove = packet.pts < *keep_from;
        if (remove) {
          discard_locked(packet);
        }
        return remove;
      });
  if (remove_begin != packets_.end()) {
    packets_.erase(remove_begin, packets_.end());
    rebuild_keyframe_indices_locked();
  }
}

void ReplayRing::rebuild_keyframe_indices_locked() {
  keyframe_pts_.clear();
  for (const auto& packet : packets_) {
    if (packet.stream == StreamKind::Video && packet.keyframe) {
      keyframe_pts_[packet.epoch].push_back(packet.pts);
    }
  }
}

}  // namespace rebelliocap
