#include <catch2/catch_test_macros.hpp>
#include "audio/pcm_normalizer.h"
#include "audio/wasapi_timeline.h"
#include "audio/pcm_epoch_bridge.h"
#include "audio/pcm_windowizer.h"
#include "audio/audio_mixer.h"
using namespace rebelliocap;
TEST_CASE("Device epoch changes preserve normalized source and mixed audio capture time") {
  enum class Change { Gap, Reset, Discontinuity };
  for (const auto change : {Change::Gap, Change::Reset, Change::Discontinuity}) {
    DYNAMIC_SECTION("device change " << static_cast<int>(change)) {
      QpcClock clock;
      const auto frequency = clock.frequency();
      const auto period = frequency / 100;
      const auto anchor = frequency;
      audio_detail::WasapiTimeline timeline(frequency);
      PcmNormalizer normalizer(clock);
      PcmEpochBridge bridge(frequency);
      SynchronizedPcmWindows windows(frequency);
      AudioMixer mixer;
      std::vector<PcmBlock> source_track, mixed_track;
      const auto sink = [&](const PcmBlock& aligned) {
        source_track.push_back(aligned);
        windows.push_system(aligned);
        auto microphone = aligned;
        microphone.stream = StreamKind::MicrophoneAudio;
        microphone.interleaved.assign(aligned.interleaved.size(), 0.F);
        microphone.silent = true;
        windows.push_microphone(microphone);
        while (auto pair = windows.pop()) {
          auto mixed = mixer.mix(pair->system, pair->microphone, 1.F, 1.F);
          REQUIRE(mixed.is_success());
          mixed_track.push_back(std::move(mixed).value());
        }
        return Result<void>::success();
      };
      const auto consume = [&](PcmBlock block) {
        auto normalized = normalizer.normalize(std::move(block), StreamKind::SystemAudio);
        REQUIRE(normalized.is_success());
        for (auto& output : normalized.value()) REQUIRE(bridge.accept(std::move(output), sink).is_success());
      };
      PcmBlock first{StreamKind::SystemAudio, anchor, 48'000, 2, std::vector<float>(960, .1F), 3};
      REQUIRE(timeline.stamp(first, 0, 480).is_success());
      consume(first);
      auto changed = first;
      changed.pts = anchor + (change == Change::Gap ? 3 : 1) * period;
      changed.interleaved.assign(960, .2F);
      changed.discontinuity = change == Change::Discontinuity;
      const std::uint64_t position = change == Change::Gap ? 1440 : change == Change::Reset ? 0 : 480;
      auto epoch = timeline.stamp(changed, position, 480);
      REQUIRE_FALSE(epoch.is_success());
      REQUIRE(epoch.error().code == "audio.epoch_required");
      // Apply the capture source's new-epoch boundary, then exercise the real
      // normalization, AAC-input bridge, windowizer, and mixer components.
      timeline = audio_detail::WasapiTimeline(frequency);
      changed.discontinuity = true;
      REQUIRE(timeline.stamp(changed, position, 480).is_success());
      consume(changed);
      auto healthy = changed;
      healthy.pts += period;
      healthy.discontinuity = false;
      healthy.interleaved.assign(960, .3F);
      REQUIRE(timeline.stamp(healthy, position + 480, 480).is_success());
      consume(healthy);
      const std::size_t count = change == Change::Gap ? 5 : 3;
      REQUIRE(source_track.size() == count);
      REQUIRE(mixed_track.size() == count);
      for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(source_track[i].pts == anchor + static_cast<QpcTicks>(i) * period);
        REQUIRE(mixed_track[i].pts == source_track[i].pts);
        REQUIRE_FALSE(source_track[i].discontinuity);
        REQUIRE_FALSE(mixed_track[i].discontinuity);
        REQUIRE(mixed_track[i].interleaved == source_track[i].interleaved);
      }
      if (change == Change::Gap) {
        REQUIRE(source_track[1].silent);
        REQUIRE(source_track[2].silent);
      }
      REQUIRE(source_track[count - 2].interleaved == std::vector<float>(960, .2F));
      REQUIRE(source_track.back().interleaved == std::vector<float>(960, .3F));
      REQUIRE(normalizer.statistics().segments == 2);
      REQUIRE(normalizer.statistics().accepted_frames == 1440);
      REQUIRE(windows.dropped_frames() == 0);
    }
  }
}
static PcmBlock packet(QpcTicks pts) { return {StreamKind::SystemAudio, pts, 48000, 2, std::vector<float>(960)}; }
TEST_CASE("Large native QPC jumps request source recovery without trusting the bad packet") {
  for (const auto raw : {QpcTicks{979999}, QpcTicks{600'000'000}}) {
    audio_detail::WasapiTimeline timeline(10'000'000);
    auto first = packet(1'000'000);
    REQUIRE(timeline.stamp(first, 0, 480).is_success());
    auto jumped = packet(raw);
    auto result = timeline.stamp(jumped, 480, 480);
    REQUIRE_FALSE(result.is_success());
    REQUIRE(result.error().code == "audio.timestamp_unreliable");
    REQUIRE_FALSE(jumped.timestamp_reconstructed);
  }
}
TEST_CASE("Device gaps request a new epoch without relaxing corrupt timestamp validation") {
  audio_detail::WasapiTimeline timeline(10'000'000);
  auto first = packet(1'000'000);
  REQUIRE(timeline.stamp(first, 0, 480).is_success());
  auto gap = packet(1'300'000);
  auto result = timeline.stamp(gap, 1440, 480);
  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "audio.epoch_required");
  timeline = audio_detail::WasapiTimeline(10'000'000);
  gap.discontinuity = true;
  REQUIRE(timeline.stamp(gap, 1440, 480).is_success());
  auto invalid = packet(-1);
  auto rejected = timeline.stamp(invalid, 1920, 480);
  REQUIRE_FALSE(rejected.is_success());
  REQUIRE(rejected.error().code == "audio.timestamp_invalid");
}
TEST_CASE("Observed contiguous480-frame backwards QPC reconstructs without dropping samples") {
  audio_detail::WasapiTimeline timeline(10000000);
  auto first = packet(791177719280);
  REQUIRE(timeline.stamp(first, 3794210880, 480).is_success());
  auto next = packet(791177719251);
  REQUIRE(timeline.stamp(next, 3794211360, 480).is_success());
  REQUIRE(next.pts == 791177819280);
  REQUIRE(next.raw_pts == 791177719251);
  REQUIRE(next.timestamp_reconstructed);
  REQUIRE(next.interleaved.size() == 960);
  auto repeated = packet(791177819241);
  REQUIRE(timeline.stamp(repeated, 3794211840, 480).is_success());
  REQUIRE(repeated.pts == 791177919280);
  auto normal = packet(791178019281);
  REQUIRE(timeline.stamp(normal, 3794212320, 480).is_success());
  REQUIRE_FALSE(normal.timestamp_reconstructed);
  REQUIRE(normal.pts == 791178019281);
}
TEST_CASE("Repeated raw QPC reconstructs a strictly increasing timestamp") {
  audio_detail::WasapiTimeline timeline(10000000);
  auto first = packet(1000000);
  REQUIRE(timeline.stamp(first, 0, 480).is_success());
  auto repeated = packet(1000000);
  REQUIRE(timeline.stamp(repeated, 480, 480).is_success());
  REQUIRE(repeated.pts == 1100000);
  REQUIRE(repeated.timestamp_reconstructed);
}
TEST_CASE("Observed bounded QPC jumps preserve contiguous device samples and the trusted anchor") {
  struct Observation {
    QpcTicks raw;
    QpcTicks predicted;
    std::uint64_t position;
  };
  for (const auto observed : {
      Observation{121668206094, 121668095698, 581356800},
      Observation{16533825213, 16533927716, 76755840}}) {
    DYNAMIC_SECTION("raw timestamp " << observed.raw) {
      audio_detail::WasapiTimeline timeline(10000000);
      auto first = packet(observed.predicted - 100000);
      REQUIRE(timeline.stamp(first, observed.position - 480, 480).is_success());

      auto jittered = packet(observed.raw);
      jittered.interleaved.assign(960, 0.25F);
      REQUIRE(timeline.stamp(jittered, observed.position, 480).is_success());
      REQUIRE(jittered.pts == observed.predicted);
      REQUIRE(jittered.raw_pts == observed.raw);
      REQUIRE(jittered.timestamp_reconstructed);
      REQUIRE(jittered.device_position == observed.position);
      REQUIRE(jittered.interleaved == std::vector<float>(960, 0.25F));

      auto healthy = packet(observed.predicted + 100000);
      REQUIRE(timeline.stamp(healthy, observed.position + 480, 480).is_success());
      REQUIRE(healthy.pts == observed.predicted + 100000);
      REQUIRE_FALSE(healthy.timestamp_reconstructed);
    }
  }
}
TEST_CASE("Observed sub-250us raw QPC regression is repaired but out-of-bound jitter fails") {
  audio_detail::WasapiTimeline accepted(10000000);
  auto first = packet(1000000);
  REQUIRE(accepted.stamp(first, 0, 480).is_success());
  auto within_tolerance = packet(998000);
  REQUIRE(accepted.stamp(within_tolerance, 480, 480).is_success());
  REQUIRE(within_tolerance.pts == 1100000);

  audio_detail::WasapiTimeline rejected(10000000);
  first = packet(1000000);
  REQUIRE(rejected.stamp(first, 0, 480).is_success());
  auto beyond_tolerance = packet(979999);
  REQUIRE_FALSE(rejected.stamp(beyond_tolerance, 480, 480).is_success());
}
TEST_CASE("Timeline rejects gaps resets large jumps and overflow") {
  for (auto position : {std::uint64_t{0}, std::uint64_t{1060}}) {
    audio_detail::WasapiTimeline t(10000000); auto a=packet(1000000); REQUIRE(t.stamp(a,100,480).is_success());
    auto b=packet(1100000); REQUIRE_FALSE(t.stamp(b,position,480).is_success());
  }
  for (auto pts : {QpcTicks{979999}, QpcTicks{2000000}}) {
    audio_detail::WasapiTimeline t(10000000); auto a=packet(1000000); REQUIRE(t.stamp(a,0,480).is_success());
    auto b=packet(pts); REQUIRE_FALSE(t.stamp(b,480,480).is_success());
  }
  audio_detail::WasapiTimeline overflow(INT64_MAX); auto a=packet(INT64_MAX-1); REQUIRE(overflow.stamp(a,0,480).is_success());
  auto b=packet(INT64_MAX-2); REQUIRE_FALSE(overflow.stamp(b,480,480).is_success());
}
TEST_CASE("QPC reconstruction is bounded for both timestamp directions") {
  for (const auto raw : {QpcTicks{1220000}, QpcTicks{980000}}) {
    DYNAMIC_SECTION("accepted boundary " << raw) {
      audio_detail::WasapiTimeline timeline(10000000);
      auto first = packet(1000000);
      REQUIRE(timeline.stamp(first, 0, 480).is_success());
      auto jittered = packet(raw);
      REQUIRE(timeline.stamp(jittered, 480, 480).is_success());
      REQUIRE(jittered.pts == 1100000);
      REQUIRE(jittered.timestamp_reconstructed);
      auto healthy = packet(1200000);
      REQUIRE(timeline.stamp(healthy, 960, 480).is_success());
      REQUIRE(healthy.pts == 1200000);
      REQUIRE_FALSE(healthy.timestamp_reconstructed);
    }
  }
  for (const auto raw : {QpcTicks{1220001}, QpcTicks{979999}}) {
    audio_detail::WasapiTimeline timeline(10000000);
    auto first = packet(1000000);
    REQUIRE(timeline.stamp(first, 0, 480).is_success());
    auto jittered = packet(raw);
    REQUIRE_FALSE(timeline.stamp(jittered, 480, 480).is_success());
  }
}
TEST_CASE("Repaired raw QPC jitter preserves normalized audio timing and every sample") {
  QpcClock clock;
  const auto period = clock.frequency() / 100;
  audio_detail::WasapiTimeline timeline(clock.frequency());
  PcmNormalizer baseline(clock), repaired(clock);
  std::vector<float> baseline_samples, repaired_samples;
  std::vector<QpcTicks> baseline_timestamps, repaired_timestamps;
  auto collect = [](const std::vector<PcmBlock>& blocks, std::vector<float>& samples,
                    std::vector<QpcTicks>& timestamps) {
    for (const auto& block : blocks) {
      timestamps.push_back(block.pts);
      samples.insert(samples.end(), block.interleaved.begin(), block.interleaved.end());
    }
  };
  for (std::uint64_t i = 0; i < 3; ++i) {
    auto healthy = packet(1000000 + static_cast<QpcTicks>(i) * period);
    healthy.interleaved.assign(960, static_cast<float>(i + 1) / 10.0F);
    healthy.device_position = i * 480;
    healthy.device_position_valid = true;
    auto expected = baseline.normalize(healthy, StreamKind::SystemAudio);
    REQUIRE(expected.is_success());
    collect(expected.value(), baseline_samples, baseline_timestamps);

    auto jittered = healthy;
    if (i == 1) {
      // The observed +110396 QPC residual at 10MHz is one 10ms packet +1.0396ms.
      jittered.pts += period + clock.frequency() * 10396 / 10000000;
    }
    REQUIRE(timeline.stamp(jittered, i * 480, 480).is_success());
    auto normalized = repaired.normalize(jittered, StreamKind::SystemAudio);
    REQUIRE(normalized.is_success());
    collect(normalized.value(), repaired_samples, repaired_timestamps);
  }
  auto expected_tail = baseline.flush();
  auto repaired_tail = repaired.flush();
  REQUIRE(expected_tail.is_success());
  REQUIRE(repaired_tail.is_success());
  collect(expected_tail.value(), baseline_samples, baseline_timestamps);
  collect(repaired_tail.value(), repaired_samples, repaired_timestamps);
  REQUIRE(repaired_samples.size() == 2880);
  REQUIRE(repaired_samples == baseline_samples);
  REQUIRE(repaired_timestamps == baseline_timestamps);
  REQUIRE(repaired.statistics().accepted_frames == 1440);
  REQUIRE(repaired.statistics().emitted_frames == 1440);
  REQUIRE(repaired.statistics().segments == 1);
  REQUIRE(repaired.statistics().reconstructed_input_blocks == 1);
}
TEST_CASE("Recovery follows contiguous device positions through sustained raw QPC lag") {
  audio_detail::WasapiTimeline t(10000000); auto a=packet(1000000); a.discontinuity=true; REQUIRE(t.stamp(a,0,480).is_success());
  for (int i=1;i<=64;++i) { auto b=packet(1000000+(i-1)*100000-29); REQUIRE(t.stamp(b,static_cast<std::uint64_t>(i)*480,480).is_success()); REQUIRE(b.pts==1000000+i*100000); }
  audio_detail::WasapiTimeline d(10000000); auto start=packet(1000000); REQUIRE(d.stamp(start,0,480).is_success());
  auto discontinuity=packet(1100000); discontinuity.discontinuity=true; REQUIRE_FALSE(d.stamp(discontinuity,480,480).is_success());
}
