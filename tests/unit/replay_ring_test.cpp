#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "media/encoded_packet.h"
#include "replay/replay_ring.h"

namespace {

using rebelliocap::EncodedPacket;
using rebelliocap::QpcTicks;
using rebelliocap::ReplayRing;
using rebelliocap::StreamKind;

EncodedPacket packet(StreamKind stream, std::uint32_t epoch, QpcTicks pts,
                     bool keyframe = false, std::size_t payload_bytes = 1) {
  return {.stream = stream,
          .epoch = epoch,
          .pts = pts,
          .dts = pts,
          .duration = 1,
          .keyframe = keyframe,
          .payload = std::make_shared<std::vector<std::byte>>(payload_bytes)};
}

EncodedPacket reordered_video_packet(std::uint32_t epoch, QpcTicks pts,
                                     QpcTicks dts, bool keyframe = false) {
  auto value = packet(StreamKind::Video, epoch, pts, keyframe);
  value.dts = dts;
  return value;
}

void append(ReplayRing& ring, EncodedPacket value) {
  const auto result = ring.append(std::move(value));
  REQUIRE(result.is_success());
}

std::size_t stream_count(const rebelliocap::ReplaySnapshot& snapshot, StreamKind stream) {
  std::size_t count = 0;
  for (const auto& item : snapshot.packets) {
    if (item.stream == stream) {
      ++count;
    }
  }
  return count;
}

}  // namespace

TEST_CASE("long duration Replay bounds encoded data instead of retaining arbitrary bytes", "[memory-budget]") {
  ReplayRing ring(3600);
  for (int i = 0; i < 40; ++i) {
    append(ring, packet(StreamKind::Video, 1, i, true, 2 * 1024 * 1024));
  }
  // The standalone ring has a conservative 64 MiB default, independent of time.
  REQUIRE(ring.bytes() <= 64 * 1024 * 1024);
  const auto clip = ring.snapshot(39, 3600);
  REQUIRE(clip.is_success());
  REQUIRE(clip.value().actual_start > 0);
  REQUIRE(clip.value().packets.front().keyframe);
}

TEST_CASE("sixteen saves keep evicted payload charged and recover after release", "[memory-budget]") {
  auto budget = std::make_shared<rebelliocap::PacketMemoryBudget>(8192);
  ReplayRing ring(3600, budget);
  append(ring, packet(StreamKind::Video, 1, 0, true, 4096));
  std::vector<rebelliocap::ReplaySnapshot> saves;
  for (int i = 0; i < 16; ++i) {
    auto save = ring.snapshot(0, 3600);
    REQUIRE(save.is_success());
    saves.push_back(std::move(save).value());
  }
  const auto pinned_bytes = budget->bytes();
  REQUIRE(pinned_bytes > 4096);
  REQUIRE(pinned_bytes <= 8192);
  const auto pressure = ring.append(packet(StreamKind::Video, 1, 1, true, 4096));
  REQUIRE_FALSE(pressure.is_success());
  REQUIRE(pressure.error().code == "replay.memory_budget_exceeded");
  REQUIRE(ring.bytes() == 0);
  REQUIRE(budget->bytes() == pinned_bytes);
  REQUIRE(saves.front().packets.front().payload->size() == 4096);
  // Metadata and shared payload are released when all saves finish.
  saves.clear();
  REQUIRE(budget->bytes() == 0);
  append(ring, packet(StreamKind::Video, 1, 2, false, 4096));
  REQUIRE(ring.bytes() == 0);
  append(ring, packet(StreamKind::Video, 1, 3, true, 4096));
  REQUIRE(ring.snapshot(3, 3600).value().actual_start == 3);
  REQUIRE(budget->bytes() <= 8192);
}

TEST_CASE("allocation capacity rather than payload size is admitted", "[memory-budget]") {
  rebelliocap::PacketMemoryBudget budget(4096);
  auto payload = std::make_shared<std::vector<std::byte>>(1);
  payload->reserve(8192);
  REQUIRE_FALSE(budget.track(payload).is_success());
  REQUIRE(budget.bytes() == 0);
}

TEST_CASE("shared tracking does not copy or double charge an allocation", "[memory-budget]") {
  rebelliocap::PacketMemoryBudget budget(8192);
  auto payload = std::make_shared<const std::vector<std::byte>>(4096);
  auto first = budget.track(payload);
  REQUIRE(first.is_success());
  const auto charged = budget.bytes();
  auto second = budget.track(first.value());
  REQUIRE(second.is_success());
  REQUIRE(second.value().get() == payload.get());
  REQUIRE(budget.bytes() == charged);
  first.value().reset();
  REQUIRE(budget.bytes() == charged);
  second.value().reset();
  REQUIRE(budget.bytes() == 0);
}

TEST_CASE("snapshot metadata admission fails without exceeding the shared budget", "[memory-budget]") {
  auto budget = std::make_shared<rebelliocap::PacketMemoryBudget>(8192);
  ReplayRing ring(30, budget);
  append(ring, packet(StreamKind::Video, 1, 0, true, 4096));
  auto reserve = budget->reserve(8192 - budget->bytes());
  REQUIRE(reserve.is_success());
  const auto snapshot = ring.snapshot(0, 30);
  REQUIRE_FALSE(snapshot.is_success());
  REQUIRE(snapshot.error().code == "replay.memory_budget_exceeded");
  REQUIRE(budget->bytes() == 8192);
}

TEST_CASE("snapshot retains accounting beyond ring destruction", "[memory-budget]") {
  auto budget = std::make_shared<rebelliocap::PacketMemoryBudget>(8192);
  std::optional<rebelliocap::ReplaySnapshot> save;
  {
    ReplayRing ring(30, budget);
    append(ring, packet(StreamKind::Video, 1, 0, true, 4096));
    save.emplace(std::move(ring.snapshot(0, 30)).value());
  }
  REQUIRE(budget->bytes() > 4096);
  REQUIRE(save->packets.front().payload->size() == 4096);
  save.reset();
  REQUIRE(budget->bytes() == 0);
}

TEST_CASE("memory warning clears when full decodable retention recovers", "[memory-budget]") {
  auto budget = std::make_shared<rebelliocap::PacketMemoryBudget>(8192);
  ReplayRing ring(5, budget);
  append(ring, packet(StreamKind::Video, 1, 0, true, 4096));
  auto save = ring.snapshot(0, 5);
  REQUIRE(save.is_success());
  REQUIRE_FALSE(ring.append(packet(StreamKind::Video, 1, 1, true, 4096)).is_success());
  REQUIRE(ring.memory_limited());
  save.value().packets.clear();
  save.value().memory_reservation.reset();
  for (int i = 2; i <= 7; ++i) {
    append(ring, packet(StreamKind::Video, 1, i, i == 2));
  }
  REQUIRE(ring.retained_duration() == 5);
  REQUIRE(ring.snapshot(7, 5).value().actual_start == 2);
  REQUIRE_FALSE(ring.memory_limited());
  REQUIRE(ring.memory_drops() > 0);
}

TEST_CASE("registry maintenance releases transient allocations beside long lived buffers", "[memory-budget][registry-profile]") {
  rebelliocap::PacketMemoryBudget budget(32 * 1024 * 1024);
  std::vector<std::shared_ptr<const std::vector<std::byte>>> retained;
  retained.reserve(25'000);
  for (int i = 0; i < 25'000; ++i) {
    auto tracked = budget.track(std::make_shared<const std::vector<std::byte>>(1));
    REQUIRE(tracked.is_success());
    retained.push_back(std::move(tracked).value());
  }
  const auto stable = budget.bytes();
  for (int i = 0; i < 100'000; ++i) {
    auto tracked = budget.track(std::make_shared<const std::vector<std::byte>>(1));
    REQUIRE(tracked.is_success());
  }
  REQUIRE(budget.bytes() == stable);
  retained.clear();
  REQUIRE(budget.bytes() == 0);
}

TEST_CASE("concurrent reservations cannot overcommit the budget", "[memory-budget]") {
  rebelliocap::PacketMemoryBudget budget(100);
  std::atomic_bool exceeded{false};
  std::vector<std::jthread> threads;
  for (int thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&] {
      for (int i = 0; i < 1000; ++i) {
        const auto reservation = budget.reserve(60);
        if (budget.bytes() > 100) exceeded = true;
        std::this_thread::yield();
      }
    });
  }
  threads.clear();
  REQUIRE_FALSE(exceeded.load());
  REQUIRE(budget.bytes() == 0);
}

TEST_CASE("memory trimming keeps the newest decode prefix and drops older audio", "[memory-budget]") {
  auto budget = std::make_shared<rebelliocap::PacketMemoryBudget>(8192);
  ReplayRing ring(3600, budget);
  auto first = reordered_video_packet(1, 0, 0, true);
  first.payload = std::make_shared<const std::vector<std::byte>>(1024);
  append(ring, std::move(first));
  auto future = reordered_video_packet(1, 3, 1);
  future.payload = std::make_shared<const std::vector<std::byte>>(1024);
  append(ring, std::move(future));
  append(ring, packet(StreamKind::SystemAudio, 1, 1, false, 1024));
  auto next = reordered_video_packet(1, 4, 2, true);
  next.payload = std::make_shared<const std::vector<std::byte>>(1024);
  append(ring, std::move(next));
  auto reference = reordered_video_packet(1, 7, 3);
  reference.payload = std::make_shared<const std::vector<std::byte>>(1024);
  append(ring, std::move(reference));
  auto reordered = reordered_video_packet(1, 5, 4);
  reordered.payload = std::make_shared<const std::vector<std::byte>>(1024);
  append(ring, std::move(reordered));
  const auto snapshot = ring.snapshot(5, 3600).value();
  REQUIRE(snapshot.actual_start == 4);
  REQUIRE(snapshot.packets.size() == 3);
  REQUIRE(snapshot.packets[0].keyframe);
  REQUIRE(snapshot.packets[1].pts == 7);
  REQUIRE(snapshot.packets[2].pts == 5);
  REQUIRE(budget->bytes() <= 8192);
}

TEST_CASE("append rejects decode timestamps that move backward within one stream") {
  ReplayRing ring(30);
  append(ring, packet(StreamKind::Video, 1, 10, true, 4));

  const auto result = ring.append(reordered_video_packet(1, 12, 9));

  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "replay.non_monotonic_packet");
  REQUIRE(ring.bytes() == 4);
}

TEST_CASE("append rejects duplicate decode timestamps within one stream") {
  ReplayRing ring(30);
  append(ring, reordered_video_packet(1, 10, 8, true));

  const auto result = ring.append(reordered_video_packet(1, 12, 8));

  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "replay.non_monotonic_packet");
  REQUIRE(ring.bytes() == 1);
}

TEST_CASE("append preserves decode order while accepting reordered video PTS") {
  ReplayRing ring(30);
  append(ring, reordered_video_packet(1, 100, 98, true));
  append(ring, reordered_video_packet(1, 103, 99));
  append(ring, reordered_video_packet(1, 101, 100));
  append(ring, reordered_video_packet(1, 102, 101));

  const auto snapshot = ring.snapshot(103, 3).value();

  REQUIRE(snapshot.packets.size() == 4);
  REQUIRE(snapshot.packets[0].dts == 98);
  REQUIRE(snapshot.packets[1].pts == 103);
  REQUIRE(snapshot.packets[2].pts == 101);
  REQUIRE(snapshot.packets[3].dts == 101);
}

TEST_CASE("a partially filled replay saves available footage before the full duration") {
  ReplayRing ring(120);
  append(ring, packet(StreamKind::Video, 1, 1000, true));
  append(ring, packet(StreamKind::SystemAudio, 1, 1001));
  append(ring, packet(StreamKind::Video, 1, 1010));
  const auto result = ring.snapshot(1010, 120);
  REQUIRE(result.is_success());
  REQUIRE(result.value().requested_start == 1000);
  REQUIRE(result.value().actual_start == 1000);
  REQUIRE(result.value().packets.size() == 3);
}

TEST_CASE("ring prunes packets before the oldest keyframe needed by capacity") {
  ReplayRing ring(30);
  append(ring, packet(StreamKind::Video, 1, 0, true));
  append(ring, packet(StreamKind::SystemAudio, 1, 29));
  append(ring, packet(StreamKind::Video, 1, 30, true));
  append(ring, packet(StreamKind::MicrophoneAudio, 1, 30));
  append(ring, packet(StreamKind::Video, 1, 60));

  const auto snapshot = ring.snapshot(60, 30).value();

  REQUIRE(snapshot.actual_start == 30);
  REQUIRE(snapshot.packets.size() == 3);
  REQUIRE(ring.bytes() == 3);
}

TEST_CASE("short epoch transitions discard stale packets without a capacity keyframe") {
  ReplayRing ring(100);
  constexpr std::uint32_t kEpochs = 100;

  for (std::uint32_t epoch = 1; epoch <= kEpochs; ++epoch) {
    append(ring, packet(StreamKind::Video, epoch, 0, true, 2));
    append(ring, packet(StreamKind::SystemAudio, epoch, 1, false, 3));
    append(ring, packet(StreamKind::Video, epoch, 2, false, 5));
  }

  const auto snapshot = ring.snapshot(2, 2).value();

  REQUIRE(snapshot.epoch == kEpochs);
  REQUIRE(snapshot.packets.size() == 3);
  REQUIRE(ring.bytes() == 10);
  REQUIRE(ring.retained_duration() == 2);
}

TEST_CASE("negative capacity clamps to zero and QPC arithmetic saturates") {
  const auto minimum = std::numeric_limits<QpcTicks>::min();
  const auto maximum = std::numeric_limits<QpcTicks>::max();
  ReplayRing ring(-1);
  append(ring, packet(StreamKind::Video, 1, minimum, true, 2));
  append(ring, packet(StreamKind::Video, 1, maximum, true, 3));

  const auto expired = ring.snapshot(minimum, 1);
  REQUIRE_FALSE(expired.is_success());
  const auto lower_bound_snapshot = ring.snapshot(maximum, maximum).value();

  REQUIRE(lower_bound_snapshot.requested_start == maximum);
  REQUIRE(ring.bytes() == 3);
  REQUIRE(ring.retained_duration() == 0);
}

TEST_CASE("snapshot starts at preceding keyframe and keeps all audio") {
  ReplayRing ring(30);
  append(ring, packet(StreamKind::Video, 1, 0, true));
  append(ring, packet(StreamKind::Video, 1, 2, true));
  append(ring, packet(StreamKind::SystemAudio, 1, 3));
  append(ring, packet(StreamKind::MicrophoneAudio, 1, 3));
  append(ring, packet(StreamKind::Video, 1, 4, true));
  append(ring, packet(StreamKind::SystemAudio, 1, 5));
  append(ring, packet(StreamKind::MicrophoneAudio, 1, 5));
  append(ring, packet(StreamKind::Video, 1, 6));

  const auto clip = ring.snapshot(6, 3).value();

  REQUIRE(clip.actual_start == 2);
  REQUIRE(stream_count(clip, StreamKind::SystemAudio) > 0);
  REQUIRE(stream_count(clip, StreamKind::MicrophoneAudio) > 0);
}

TEST_CASE("snapshot never mixes packets from separate epochs") {
  ReplayRing ring(30);
  append(ring, packet(StreamKind::Video, 1, 0, true));
  append(ring, packet(StreamKind::SystemAudio, 1, 1));
  append(ring, packet(StreamKind::Video, 1, 2));
  append(ring, packet(StreamKind::Video, 2, 0, true));
  append(ring, packet(StreamKind::MicrophoneAudio, 2, 1));
  append(ring, packet(StreamKind::Video, 2, 2));

  const auto clip = ring.snapshot(2, 1).value();

  REQUIRE(clip.epoch == 2);
  REQUIRE(clip.actual_start == 0);
  REQUIRE(stream_count(clip, StreamKind::SystemAudio) == 0);
  REQUIRE(stream_count(clip, StreamKind::MicrophoneAudio) == 1);
}

TEST_CASE("snapshot owns packet metadata after later appends") {
  ReplayRing ring(30);
  append(ring, packet(StreamKind::Video, 1, 0, true));
  append(ring, packet(StreamKind::Video, 1, 1));

  const auto snapshot = ring.snapshot(1, 1).value();
  append(ring, packet(StreamKind::SystemAudio, 1, 1));
  append(ring, packet(StreamKind::Video, 1, 2, true));

  REQUIRE(snapshot.packets.size() == 2);
  REQUIRE(snapshot.end == 1);
  REQUIRE(ring.bytes() == 4);
}

TEST_CASE("snapshot refuses a video range without a preceding keyframe") {
  ReplayRing ring(30);
  append(ring, packet(StreamKind::Video, 1, 5));

  const auto result = ring.snapshot(5, 2);

  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "replay.keyframe_unavailable");
}

TEST_CASE("concurrent append and snapshot 10000 iterations has no MSVC ThreadSanitizer coverage") {
  constexpr int kIterations = 10'000;
  ReplayRing ring(kIterations + 1);
  append(ring, packet(StreamKind::Video, 1, 0, true));

  std::atomic_bool start{false};
  std::atomic_bool passed{true};
  std::thread producer([&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (int i = 1; i <= kIterations; ++i) {
      const auto result = ring.append(packet(StreamKind::Video, 1, i, i % 60 == 0));
      if (!result.is_success()) {
        passed.store(false, std::memory_order_release);
        return;
      }
    }
  });

  std::thread consumer([&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < kIterations; ++i) {
      const auto result = ring.snapshot(kIterations, kIterations);
      if (!result.is_success() || result.value().epoch != 1) {
        passed.store(false, std::memory_order_release);
        return;
      }
      const auto packet_count = result.value().packets.size();
      std::this_thread::yield();
      if (result.value().packets.size() != packet_count) {
        passed.store(false, std::memory_order_release);
        return;
      }
    }
  });

  start.store(true, std::memory_order_release);
  producer.join();
  consumer.join();

  REQUIRE(passed.load(std::memory_order_acquire));
  REQUIRE(ring.snapshot(kIterations, kIterations).is_success());
}

TEST_CASE("snapshot retains decode references beyond presentation cutoff") {
  ReplayRing ring(30);
  append(ring, reordered_video_packet(1, 100, 98, true));
  append(ring, reordered_video_packet(1, 103, 99));
  append(ring, reordered_video_packet(1, 101, 100));
  append(ring, packet(StreamKind::SystemAudio, 1, 102));
  append(ring, packet(StreamKind::SystemAudio, 1, 103));
  append(ring, reordered_video_packet(1, 102, 101));
  append(ring, reordered_video_packet(1, 106, 102));
  const auto snapshot = ring.snapshot(102, 2).value();
  REQUIRE(snapshot.end == 102);
  REQUIRE(stream_count(snapshot, StreamKind::Video) == 4);
  REQUIRE(stream_count(snapshot, StreamKind::SystemAudio) == 1);
  REQUIRE(snapshot.packets[1].pts == 103);
  REQUIRE(snapshot.packets.back().pts == 102);
}

TEST_CASE("late packets below an unchanged replay boundary are discarded") {
  ReplayRing ring(30);
  append(ring, reordered_video_packet(1, 0, 0, true));
  append(ring, reordered_video_packet(1, 30, 1, true));
  append(ring, reordered_video_packet(1, 60, 2));
  append(ring, reordered_video_packet(1, 29, 3));
  append(ring, packet(StreamKind::SystemAudio, 1, 29));

  const auto snapshot = ring.snapshot(60, 30).value();

  REQUIRE(ring.bytes() == 2);
  REQUIRE(snapshot.packets.size() == 2);
  REQUIRE(snapshot.actual_start == 30);
  REQUIRE(snapshot.packets[0].pts == 30);
  REQUIRE(snapshot.packets[1].pts == 60);
}

TEST_CASE("two hour audio outage expires video and stops retaining audio past Replay capacity") {
  auto budget = std::make_shared<rebelliocap::PacketMemoryBudget>(16 * 1024 * 1024);
  ReplayRing ring(500, budget);
  append(ring, packet(StreamKind::Video, 1, 0, true, 1024));
  for (int second = 1; second <= 7200; ++second) {
    for (const auto stream : {StreamKind::SystemAudio, StreamKind::MicrophoneAudio, StreamKind::MixedAudio})
      append(ring, packet(stream, 1, second, false, 32));
    REQUIRE(ring.retained_duration() <= 500);
    if (second > 500) {
      REQUIRE(ring.bytes() == 0);
      REQUIRE(budget->bytes() == 0);
    }
  }
  REQUIRE_FALSE(ring.memory_limited());
  REQUIRE(ring.memory_drops() == 0);
  auto unavailable = ring.snapshot(7200, 500);
  REQUIRE_FALSE(unavailable.is_success());
  REQUIRE(unavailable.error().code == "replay.keyframe_unavailable");
  append(ring, packet(StreamKind::Video, 1, 7201, false));
  REQUIRE(ring.bytes() == 0);
  append(ring, packet(StreamKind::Video, 1, 7202, true, 1024));
  append(ring, packet(StreamKind::SystemAudio, 1, 7202, false, 32));
  append(ring, reordered_video_packet(1, 7204, 7203));
  append(ring, reordered_video_packet(1, 7203, 7204));
  auto restored = ring.snapshot(7203, 500);
  REQUIRE(restored.is_success());
  REQUIRE(restored.value().actual_start == 7202);
  REQUIRE(stream_count(restored.value(), StreamKind::Video) == 3);
  REQUIRE(stream_count(restored.value(), StreamKind::SystemAudio) == 1);
}

TEST_CASE("audio before any video cannot grow past the Replay window") {
  ReplayRing ring(10);
  for (int second = 0; second <= 100; ++second) {
    append(ring, packet(StreamKind::SystemAudio, 1, second));
    REQUIRE(ring.retained_duration() <= 10);
    if (second > 10) REQUIRE(ring.bytes() == 0);
  }
  append(ring, packet(StreamKind::Video, 2, 0, true));
  REQUIRE(ring.snapshot(0, 10).is_success());
}

TEST_CASE("time expiry preserves outstanding saves and rejects stale recovery keyframes") {
  auto budget = std::make_shared<rebelliocap::PacketMemoryBudget>(16 * 1024 * 1024);
  ReplayRing ring(10, budget);
  append(ring, packet(StreamKind::Video, 1, 0, true, 1024));
  auto saved = ring.snapshot(0, 10);
  REQUIRE(saved.is_success());
  const auto charged = budget->bytes();
  append(ring, packet(StreamKind::SystemAudio, 1, 11));
  REQUIRE(ring.bytes() == 0);
  REQUIRE(budget->bytes() == charged);
  REQUIRE(saved.value().packets.front().payload->size() == 1024);
  append(ring, packet(StreamKind::SystemAudio, 1, 100));
  append(ring, reordered_video_packet(1, 50, 1, true));
  REQUIRE(ring.bytes() == 0);
  append(ring, reordered_video_packet(1, 100, 2, true));
  append(ring, packet(StreamKind::MicrophoneAudio, 1, 99));
  REQUIRE(ring.bytes() == 1);
  REQUIRE(ring.snapshot(100, 10).value().actual_start == 100);
  REQUIRE(saved.value().packets.front().pts == 0);
}

TEST_CASE("replay pruning follows the maximum PTS across reordered packets and audio") {
  ReplayRing ring(30);
  append(ring, reordered_video_packet(1, 0, 0, true));
  append(ring, reordered_video_packet(1, 30, 1, true));
  append(ring, reordered_video_packet(1, 40, 2, true));
  append(ring, packet(StreamKind::SystemAudio, 1, 70));
  append(ring, reordered_video_packet(1, 35, 3));

  const auto snapshot = ring.snapshot(70, 30).value();

  REQUIRE(ring.bytes() == 2);
  REQUIRE(ring.retained_duration() == 30);
  REQUIRE(snapshot.actual_start == 40);
  REQUIRE(snapshot.packets.size() == 2);
  REQUIRE(snapshot.packets[0].pts == 40);
  REQUIRE(snapshot.packets[1].pts == 70);
}

TEST_CASE("delayed keyframes retain the original append order pruning semantics") {
  ReplayRing ring(30);
  append(ring, reordered_video_packet(1, 0, 0, true));
  append(ring, reordered_video_packet(1, 30, 1, true));
  append(ring, reordered_video_packet(1, 60, 2));
  append(ring, reordered_video_packet(1, 20, 3, true));
  append(ring, packet(StreamKind::SystemAudio, 1, 19));
  append(ring, packet(StreamKind::SystemAudio, 1, 25));

  const auto snapshot = ring.snapshot(60, 40).value();

  REQUIRE(ring.bytes() == 4);
  REQUIRE(ring.retained_duration() == 40);
  REQUIRE(snapshot.actual_start == 20);
  REQUIRE(snapshot.packets.size() == 2);
  REQUIRE(snapshot.packets[0].pts == 20);
  REQUIRE(snapshot.packets[1].pts == 25);
}

TEST_CASE("an audio first epoch transition resets the replay pruning timeline") {
  ReplayRing ring(30);
  append(ring, packet(StreamKind::Video, 1, 1000, true, 2));
  append(ring, packet(StreamKind::Video, 1, 1040, false, 3));
  append(ring, packet(StreamKind::SystemAudio, 2, 1, false, 5));
  append(ring, packet(StreamKind::Video, 2, 0, true, 7));

  const auto snapshot = ring.snapshot(1, 1).value();

  REQUIRE(snapshot.epoch == 2);
  REQUIRE(snapshot.actual_start == 0);
  REQUIRE(snapshot.packets.size() == 2);
  REQUIRE(ring.bytes() == 12);
  REQUIRE(ring.retained_duration() == 1);
}
