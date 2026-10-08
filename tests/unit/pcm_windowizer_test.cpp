#include <catch2/catch_test_macros.hpp>

#include "audio/pcm_windowizer.h"
#include "audio/audio_mixer.h"

using namespace rebelliocap;

namespace {

PcmBlock block(StreamKind stream, QpcTicks pts, std::size_t frames, float sample) {
  return {stream, pts, 48'000, 2, std::vector<float>(frames * 2, sample), 3};
}

}  // namespace

TEST_CASE("synchronized audio windows do not discard the leading source") {
  SynchronizedPcmWindows windows(10'000'000);
  windows.push_system(block(StreamKind::SystemAudio, 100, 480, 0.25F));

  REQUIRE_FALSE(windows.pop().has_value());

  windows.push_microphone(block(StreamKind::MicrophoneAudio, 200, 480, 0.5F));
  const auto pair = windows.pop();
  REQUIRE(pair.has_value());
  REQUIRE(pair->system.pts == 100);
  REQUIRE(pair->microphone.pts == 100); // Sub-sample timestamp jitter rounds to the common grid.
  REQUIRE(pair->system.interleaved.front() == 0.25F);
  REQUIRE(pair->microphone.interleaved.front() == 0.5F);
}

TEST_CASE("audio windows preserve ten millisecond QPC cadence") {
  SynchronizedPcmWindows windows(10'000'000);
  windows.push_system(block(StreamKind::SystemAudio, 1'000, 960, 0.25F));
  windows.push_microphone(block(StreamKind::MicrophoneAudio, 2'000, 960, 0.5F));

  const auto first = windows.pop();
  const auto second = windows.pop();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(second->system.pts - first->system.pts == 100'000);
  REQUIRE(second->microphone.pts - first->microphone.pts == 100'000);
}

TEST_CASE("missing system audio does not stall microphone mixing for minutes") {
  SynchronizedPcmWindows windows(10'000'000);
  std::size_t emitted = 0;
  for (int i = 0; i < 30'000; ++i) {
    windows.push_microphone(block(StreamKind::MicrophoneAudio, 1'000 + i * 100'000LL, 480, .25F));
    while (auto pair = windows.pop()) {
      REQUIRE(pair->system.silent);
      REQUIRE(pair->system.interleaved == std::vector<float>(960, 0));
      REQUIRE(pair->microphone.interleaved == std::vector<float>(960, .25F));
      ++emitted;
    }
    if (i == 99) REQUIRE(emitted >= 75);
  }
  REQUIRE(emitted >= 29'975);
}

TEST_CASE("missing microphone is replaced by silence after bounded wait") {
  SynchronizedPcmWindows windows(10'000'000);
  for (int i = 0; i < 25; ++i)
    windows.push_system(block(StreamKind::SystemAudio, 1'000 + i * 100'000LL, 480, .25F));
  const auto pair = windows.pop();
  REQUIRE(pair);
  REQUIRE(pair->system.interleaved.front() == .25F);
  REQUIRE(pair->microphone.silent);
  REQUIRE(pair->microphone.interleaved == std::vector<float>(960, 0));
}

TEST_CASE("mixing queues remain bounded even without a consumer") {
  SynchronizedPcmWindows windows(10'000'000);
  windows.push_system(block(StreamKind::SystemAudio, 1'000, 480'000, .25F));
  REQUIRE(windows.buffered_samples() <= 48'000);
  REQUIRE(windows.dropped_frames() == 456'000);
  const auto pair = windows.pop();
  REQUIRE(pair);
  REQUIRE(pair->system.discontinuity);
  REQUIRE(pair->system.pts == 95'001'000);
}

TEST_CASE("returning audio cannot replay samples already replaced by silence") {
  SynchronizedPcmWindows windows(10'000'000);
  for (int i = 0; i < 30; ++i) {
    windows.push_microphone(block(StreamKind::MicrophoneAudio, 1'000 + i * 100'000LL, 480, .25F));
    while (windows.pop()) {}
  }
  windows.push_system(block(StreamKind::SystemAudio, 1'000, 480 * 7, .5F));
  const auto pair = windows.pop();
  REQUIRE(pair);
  REQUIRE(pair->system.pts == 601'000);
  REQUIRE(pair->system.interleaved == std::vector<float>(960, .5F));
  REQUIRE(pair->microphone.pts == 601'000);
}

TEST_CASE("silence padding preserves a partial real source window") {
  SynchronizedPcmWindows windows(10'000'000);
  windows.push_system(block(StreamKind::SystemAudio, 1'000, 240, .5F));
  windows.push_microphone(block(StreamKind::MicrophoneAudio, 1'000, 12'000, .25F));
  const auto pair = windows.pop();
  REQUIRE(pair);
  REQUIRE_FALSE(pair->system.silent);
  REQUIRE(pair->system.interleaved[479] == .5F);
  REQUIRE(pair->system.interleaved[480] == 0.F);
}

TEST_CASE("stopping mixing drains the bounded wait and pads the final window") {
  SynchronizedPcmWindows windows(10'000'000);
  windows.push_microphone(block(StreamKind::MicrophoneAudio, 1'000, 720, .25F));
  REQUIRE_FALSE(windows.pop());
  auto first = windows.pop(true);
  REQUIRE(first);
  REQUIRE(first->system.silent);
  REQUIRE(first->microphone.interleaved == std::vector<float>(960, .25F));
  auto last = windows.pop(true);
  REQUIRE(last);
  REQUIRE(last->microphone.interleaved[479] == .25F);
  REQUIRE(last->microphone.interleaved[480] == 0.F);
  REQUIRE_FALSE(windows.pop(true));
}

TEST_CASE("irregular live blocks preserve microphone samples while peer is missing") {
  SynchronizedPcmWindows windows(10'000'000);
  std::size_t received = 0;
  std::uint64_t sent = 0;
  for (int i = 0; i < 100; ++i) {
    auto input = block(StreamKind::MicrophoneAudio,
        1'000 + static_cast<QpcTicks>(sent * 10'000'000 / 48'000), 997, .25F);
    windows.push_microphone(input);
    sent += 997;
    while (auto pair = windows.pop()) {
      REQUIRE_FALSE(pair->microphone.discontinuity);
      received += pair->microphone.interleaved.size() / 2;
    }
  }
  while (auto pair = windows.pop(true)) received += 480;
  REQUIRE(windows.dropped_frames() == 0);
  REQUIRE(received >= 99'700);
  REQUIRE(received < 100'180);
}

TEST_CASE("QPC alignment puts simultaneous impulses at the same mixed sample") {
  SynchronizedPcmWindows windows(10'000'000);
  auto system = block(StreamKind::SystemAudio, 1'000, 960, 0.F);
  auto microphone = block(StreamKind::MicrophoneAudio, 51'000, 720, 0.F);
  system.interleaved[600] = 1.F; // 300th frame, 6.25 ms after the common start.
  microphone.interleaved[120] = 1.F; // Starts 5 ms later, same physical impulse.
  windows.push_system(system);
  windows.push_microphone(microphone);
  const auto pair = windows.pop();
  REQUIRE(pair);
  REQUIRE(pair->system.pts == 1'000);
  REQUIRE(pair->microphone.pts == 1'000);
  REQUIRE(pair->system.interleaved[600] == 1.F);
  REQUIRE(pair->microphone.interleaved[600] == 1.F);
  REQUIRE(pair->microphone.interleaved[120] == 0.F);
  REQUIRE(microphone.interleaved[120] == 1.F); // Input/source track is untouched.
  const auto mixed = AudioMixer{}.mix(pair->system, pair->microphone, .25F, .25F);
  REQUIRE(mixed.is_success());
  REQUIRE(mixed.value().interleaved[600] == .5F);
  REQUIRE(mixed.value().interleaved[120] == 0.F);
}

TEST_CASE("QPC gap preserves silence instead of moving later PCM earlier") {
  SynchronizedPcmWindows windows(10'000'000);
  windows.push_system(block(StreamKind::SystemAudio, 1'000, 480, .25F));
  windows.push_system(block(StreamKind::SystemAudio, 201'000, 480, .75F));
  windows.push_microphone(block(StreamKind::MicrophoneAudio, 1'000, 1440, .5F));
  REQUIRE(windows.pop());
  const auto gap = windows.pop();
  REQUIRE(gap);
  REQUIRE(gap->system.interleaved == std::vector<float>(960, 0.F));
  const auto resumed = windows.pop();
  REQUIRE(resumed);
  REQUIRE(resumed->system.pts == 201'000);
  REQUIRE(resumed->system.interleaved == std::vector<float>(960, .75F));
}

TEST_CASE("QPC overlap cannot duplicate previously queued PCM") {
  SynchronizedPcmWindows windows(10'000'000);
  windows.push_system(block(StreamKind::SystemAudio, 1'000, 480, .25F));
  windows.push_system(block(StreamKind::SystemAudio, 51'000, 480, .75F));
  windows.push_microphone(block(StreamKind::MicrophoneAudio, 1'000, 720, .5F));
  REQUIRE(windows.pop());
  const auto tail = windows.pop(true);
  REQUIRE(tail);
  REQUIRE(tail->system.interleaved[479] == .75F);
  REQUIRE(tail->system.interleaved[480] == 0.F);
  REQUIRE_FALSE(windows.pop(true));
}

TEST_CASE("mixed QPC cadence retains fractional ticks over many windows") {
  SynchronizedPcmWindows windows(10'000'003);
  for (int i = 0; i <= 1000; ++i) {
    const auto pts = 1'000 + static_cast<QpcTicks>(i) * 10'000'003 / 100;
    windows.push_system(block(StreamKind::SystemAudio, pts, 480, .25F));
    windows.push_microphone(block(StreamKind::MicrophoneAudio, pts, 480, .5F));
    const auto pair = windows.pop();
    REQUIRE(pair);
    REQUIRE(pair->system.pts == pts);
    REQUIRE(pair->microphone.pts == pts);
  }
}

TEST_CASE("earlier QPC source aligns even when delivered second") {
  SynchronizedPcmWindows windows(10'000'000);
  windows.push_system(block(StreamKind::SystemAudio, 51'000, 720, .25F));
  windows.push_microphone(block(StreamKind::MicrophoneAudio, 1'000, 960, .5F));
  const auto pair = windows.pop();
  REQUIRE(pair);
  REQUIRE(pair->system.pts == 1'000);
  REQUIRE(pair->microphone.pts == 1'000);
  REQUIRE(pair->system.interleaved[479] == 0.F);
  REQUIRE(pair->system.interleaved[480] == .25F);
}

TEST_CASE("QPC offset spanning several windows preserves the leading track") {
  SynchronizedPcmWindows windows(10'000'000);
  windows.push_system(block(StreamKind::SystemAudio, 1'000, 1920, .25F));
  windows.push_microphone(block(StreamKind::MicrophoneAudio, 251'000, 720, .5F));
  for (int i = 0; i < 2; ++i) {
    const auto pair = windows.pop();
    REQUIRE(pair);
    REQUIRE(pair->microphone.silent);
    REQUIRE(pair->system.interleaved.front() == .25F);
  }
  const auto third = windows.pop();
  REQUIRE(third);
  REQUIRE(third->microphone.interleaved[479] == 0.F);
  REQUIRE(third->microphone.interleaved[480] == .5F);
}

TEST_CASE("huge QPC gap has bounded mixing storage and drain work") {
  SynchronizedPcmWindows windows(10'000'000);
  windows.push_system(block(StreamKind::SystemAudio, 1'000, 480, .25F));
  windows.push_microphone(block(StreamKind::MicrophoneAudio, 1'000, 480, .5F));
  REQUIRE(windows.pop());
  windows.push_system(block(StreamKind::SystemAudio, 6'000'001'000LL, 480, .75F));
  windows.push_microphone(block(StreamKind::MicrophoneAudio, 6'000'001'000LL, 480, .5F));
  REQUIRE(windows.buffered_samples() <= 96'000);
  const auto resumed = windows.pop(true);
  REQUIRE(resumed);
  REQUIRE(resumed->system.pts == 6'000'001'000LL);
  REQUIRE(resumed->microphone.interleaved.front() == .5F);
  REQUIRE_FALSE(windows.pop(true));
}
