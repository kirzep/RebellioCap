#include <catch2/catch_test_macros.hpp>
#include "audio/recovering_audio_source.h"
#include "audio/pcm_epoch_bridge.h"
using namespace rebelliocap;
namespace {
class Source final : public IAudioCaptureSource {
 public:
  explicit Source(bool lost) : lost_(lost) {}
  Result<std::optional<PcmBlock>> next_block(std::chrono::milliseconds) override {
    if (lost_) return Result<std::optional<PcmBlock>>::failure({"audio.endpoint_lost", "unplugged", {}});
    return Result<std::optional<PcmBlock>>::success(PcmBlock{
      StreamKind::MicrophoneAudio, 10'000'000, 48'000, 2, std::vector<float>(960, .25F), 3});
  }
 private: bool lost_;
};
class ErrorSource final : public IAudioCaptureSource {
 public:
  explicit ErrorSource(Error error) : error_(std::move(error)) {}
  Result<std::optional<PcmBlock>> next_block(std::chrono::milliseconds) override {
    return Result<std::optional<PcmBlock>>::failure(error_);
  }
 private: Error error_;
};
}
TEST_CASE("Unreliable audio timestamps enter paced pinned recovery and restore a fresh epoch") {
  QpcTicks now = 0;
  int attempts = 0;
  RecoveringAudioSource source(std::make_unique<ErrorSource>(Error{
      "audio.timestamp_unreliable", "Windows timestamp became uncertain", {}}), [&] {
    ++attempts;
    return RecoveringAudioSource::SourceResult::success(std::make_unique<Source>(false));
  }, StreamKind::SystemAudio, 10'000'000, [&] { return now; },
  [&](auto timeout) { now += timeout.count() * 10'000; });
  for (int i = 0; i < 200; ++i) {
    auto result = source.next_block(std::chrono::milliseconds(5));
    REQUIRE(result.is_success());
    REQUIRE(source.recovering());
    if (result.value()) REQUIRE(result.value()->silent);
  }
  REQUIRE(attempts == 0);
  auto restored = source.next_block(std::chrono::milliseconds(5));
  REQUIRE(restored.is_success());
  REQUIRE(restored.value());
  REQUIRE(restored.value()->discontinuity);
  REQUIRE_FALSE(source.recovering());
  REQUIRE(attempts == 1);
}
TEST_CASE("Audio epoch bridge pads capture gaps and trims recovered overlap") {
  PcmEpochBridge bridge(10'000'000);
  std::vector<PcmBlock> output;
  const auto sink = [&](const PcmBlock& b) {
    output.push_back(b); return Result<void>::success();
  };
  PcmBlock first{StreamKind::MicrophoneAudio, 1'000'000, 48'000, 2, std::vector<float>(960, .1F), 3};
  REQUIRE(bridge.accept(first, sink).is_success());
  auto recovered = first;
  recovered.pts = 1'300'000;
  recovered.discontinuity = true;
  recovered.interleaved.assign(960, .2F);
  REQUIRE(bridge.accept(recovered, sink).is_success());
  REQUIRE(output.size() == 4);
  REQUIRE(output[1].silent);
  REQUIRE(output[2].silent);
  REQUIRE(output[3].interleaved.front() == .2F);
  recovered.pts = 1'350'000;
  REQUIRE(bridge.accept(recovered, sink).is_success());
  REQUIRE(output.size() == 5);
  REQUIRE(output.back().interleaved.size() == 480);
  REQUIRE(output.back().pts == 1'400'000);
  for (std::size_t i = 1; i < output.size(); ++i) {
    REQUIRE_FALSE(output[i].discontinuity);
    REQUIRE(output[i].pts == output[i-1].pts + static_cast<QpcTicks>(output[i-1].interleaved.size() / 2) * 10'000'000 / 48'000);
    REQUIRE(output[i].mixing_pts == output[i].pts);
  }
}
TEST_CASE("Lost pinned audio endpoint emits paced silence and resumes after retry") {
  QpcTicks now = 0;
  int attempts = 0;
  RecoveringAudioSource source(std::make_unique<Source>(true), [&] {
    ++attempts;
    return RecoveringAudioSource::SourceResult::success(std::make_unique<Source>(false));
  }, StreamKind::MicrophoneAudio, 10'000'000, [&] { return now; },
  [&](auto timeout) { now += timeout.count() * 10'000; });
  auto first = source.next_block(std::chrono::milliseconds(5));
  REQUIRE(first.is_success());
  REQUIRE(source.recovering());
  for (int i = 0; i < 190; ++i) {
    auto block = source.next_block(std::chrono::milliseconds(5));
    REQUIRE(block.is_success());
    if (block.value()) {
      REQUIRE(block.value()->silent);
      REQUIRE(block.value()->interleaved.size() <= 960);
      REQUIRE(block.value()->stream == StreamKind::MicrophoneAudio);
    }
  }
  REQUIRE(attempts == 0);
  now = 10'000'000;
  auto restored = source.next_block(std::chrono::milliseconds(5));
  REQUIRE(restored.is_success());
  REQUIRE(restored.value());
  REQUIRE_FALSE(restored.value()->silent);
  REQUIRE(restored.value()->discontinuity);
  REQUIRE_FALSE(source.recovering());
  REQUIRE(attempts == 1);
}
TEST_CASE("Audio epoch bridge keeps trimming multiple stale recovered blocks") {
  PcmEpochBridge bridge(10'000'000);
  std::vector<PcmBlock> output;
  const auto sink = [&](const PcmBlock& b) { output.push_back(b); return Result<void>::success(); };
  PcmBlock b{StreamKind::MicrophoneAudio, 1'000'000, 48'000, 2, std::vector<float>(960, .25F), 3};
  REQUIRE(bridge.accept(b, sink).is_success());
  b.pts = 1'400'000; b.discontinuity = true;
  REQUIRE(bridge.accept(b, sink).is_success());
  const auto count = output.size();
  b.pts = 1'100'000;
  REQUIRE(bridge.accept(b, sink).is_success());
  b.pts = 1'200'000; b.discontinuity = false;
  REQUIRE(bridge.accept(b, sink).is_success());
  REQUIRE(output.size() == count);
}
TEST_CASE("Unavailable pinned endpoint retries at most once per second for a long outage") {
  QpcTicks now = 0;
  int attempts = 0;
  RecoveringAudioSource source(std::make_unique<Source>(true), [&] {
    ++attempts;
    return RecoveringAudioSource::SourceResult::failure({"audio.endpoint_unavailable", "still unplugged", {}});
  }, StreamKind::SystemAudio, 10'000'000, [&] { return now; },
  [&](auto timeout) { now += timeout.count() * 10'000; });
  std::optional<QpcTicks> previous;
  for (int i = 0; i < 12'000; ++i) {
    auto result = source.next_block(std::chrono::milliseconds(5));
    REQUIRE(result.is_success());
    if (result.value()) {
      const auto& block = *result.value();
      REQUIRE(block.silent);
      REQUIRE(block.interleaved.size() == 960);
      if (previous) REQUIRE(block.pts == *previous + 100'000);
      previous = block.pts;
    }
  }
  REQUIRE(source.recovering());
  REQUIRE(attempts == 59);
  REQUIRE(now == 600'000'000);
}
TEST_CASE("Unexpected audio faults preserve diagnostics and never start recovery") {
  int attempts = 0;
  RecoveringAudioSource source(std::make_unique<ErrorSource>(Error{"audio.access_denied", "privacy denied", 123L}), [&] {
    ++attempts;
    return RecoveringAudioSource::SourceResult::success(std::make_unique<Source>(false));
  }, StreamKind::MicrophoneAudio, 10'000'000, [] { return QpcTicks{0}; }, [](auto) {});
  auto result = source.next_block(std::chrono::milliseconds(5));
  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "audio.access_denied");
  REQUIRE(result.error().hresult == 123L);
  REQUIRE_FALSE(source.recovering());
  REQUIRE(attempts == 0);
}
TEST_CASE("Audio epoch bridge bounds catch-up work and propagates encoder failure") {
  PcmEpochBridge bridge(10'000'000);
  int calls = 0;
  const auto sink = [&](const PcmBlock&) { ++calls; return Result<void>::success(); };
  PcmBlock b{StreamKind::SystemAudio, 0, 48'000, 2, std::vector<float>(960), 3};
  REQUIRE(bridge.accept(b, sink).is_success());
  b.pts = 30'000'000; b.discontinuity = true;
  auto large = bridge.accept(b, sink);
  REQUIRE_FALSE(large.is_success());
  REQUIRE(large.error().code == "audio.recovery_gap_too_large");
  REQUIRE(calls == 1);
  b.pts = 100'000; b.discontinuity = false;
  auto failed = bridge.accept(b, [](const PcmBlock&) {
    return Result<void>::failure({"audio.encoder_failed", "encode failed", {}});
  });
  REQUIRE_FALSE(failed.is_success());
  REQUIRE(failed.error().code == "audio.encoder_failed");
}
TEST_CASE("Healthy loopback idle longer than two seconds remains continuous on resume") {
  struct IdleSource final : IAudioCaptureSource {
    QpcTicks& now; bool& idle;
    IdleSource(QpcTicks& time, bool& quiet) : now(time), idle(quiet) {}
    Result<std::optional<PcmBlock>> next_block(std::chrono::milliseconds) override {
      if (idle) return Result<std::optional<PcmBlock>>::success(std::nullopt);
      return Result<std::optional<PcmBlock>>::success(PcmBlock{
        StreamKind::SystemAudio, now, 48'000, 2, std::vector<float>(960, .25F), 3});
    }
  };
  QpcTicks now = 0; bool idle = false;
  RecoveringAudioSource source(std::make_unique<IdleSource>(now, idle), [] {
    return RecoveringAudioSource::SourceResult::failure({"audio.endpoint_unavailable", "unexpected retry", {}});
  }, StreamKind::SystemAudio, 10'000'000, [&] { return now; }, [](auto) {});
  PcmEpochBridge bridge(10'000'000);
  std::uint64_t frames = 0;
  const auto sink = [&](const PcmBlock& b) { frames += b.interleaved.size() / 2; return Result<void>::success(); };
  auto first = source.next_block(std::chrono::milliseconds(5));
  REQUIRE(first.value()); REQUIRE(bridge.accept(*first.value(), sink).is_success());
  idle = true;
  for (int i = 1; i <= 800; ++i) {
    now = i * 50'000;
    auto block = source.next_block(std::chrono::milliseconds(5));
    REQUIRE(block.is_success()); REQUIRE_FALSE(source.recovering());
    if (block.value()) REQUIRE(bridge.accept(*block.value(), sink).is_success());
  }
  idle = false;
  auto resumed = source.next_block(std::chrono::milliseconds(5));
  REQUIRE(resumed.value());
  REQUIRE(bridge.accept(*resumed.value(), sink).is_success());
  REQUIRE(frames == 192'480);
}
