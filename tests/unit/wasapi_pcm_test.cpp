#include <catch2/catch_test_macros.hpp>
#include <audioclient.h>
#include <limits>
#include <ksmedia.h>
#include "audio/wasapi_pcm.h"
using namespace rebelliocap;
TEST_CASE("Uncertain WASAPI timestamp requests stream recovery instead of fatal validation") {
  WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0};
  auto result = audio_detail::decode_packet(format, nullptr, 480,
      AUDCLNT_BUFFERFLAGS_SILENT | AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR,
      UINT64_MAX, 10'000'000, StreamKind::SystemAudio);
  REQUIRE_FALSE(result.is_success());
  REQUIRE(result.error().code == "audio.timestamp_unreliable");
  auto corrupt = audio_detail::qpc_from_100ns(UINT64_MAX, INT64_MAX);
  REQUIRE_FALSE(corrupt.is_success());
  REQUIRE(corrupt.error().code == "audio.timestamp_invalid");
}
TEST_CASE("WASAPI 100ns converts to raw signed QPC without whole-product overflow") {
  REQUIRE(audio_detail::qpc_from_100ns(15000001, 3000000).value() == 4500000);
  REQUIRE(audio_detail::qpc_from_100ns(10000000000000ULL, 24000000).value() == 24000000000000LL);
  REQUIRE_FALSE(audio_detail::qpc_from_100ns(UINT64_MAX, INT64_MAX).is_success());
  REQUIRE_FALSE(audio_detail::qpc_from_100ns(10, 0).is_success());
}
TEST_CASE("Extensible float and left-aligned PCM retain channel mask") {
  WAVEFORMATEXTENSIBLE ext{};
  ext.Format = {WAVE_FORMAT_EXTENSIBLE, 2, 48000, 384000, 8, 32, 22};
  ext.Samples.wValidBitsPerSample = 32;
  ext.dwChannelMask = 3;
  ext.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  const float samples[] = {0.25f, -0.5f};
  auto f = audio_detail::decode_packet(ext.Format, reinterpret_cast<const BYTE*>(samples), 1, 0, 0, 10, StreamKind::SystemAudio);
  REQUIRE(f.is_success());
  REQUIRE(f.value().channel_mask == 3);
  REQUIRE(f.value().interleaved == std::vector<float>{0.25f, -0.5f});
  ext.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
  ext.Samples.wValidBitsPerSample = 24;
  const std::int32_t pcm[] = {INT32_MIN, 0x40000000};
  auto p = audio_detail::decode_packet(ext.Format, reinterpret_cast<const BYTE*>(pcm), 1, 0, 0, 10, StreamKind::SystemAudio);
  REQUIRE(p.is_success());
  REQUIRE(p.value().interleaved == std::vector<float>{-1, 0.5f});
  ext.Format.cbSize = 0;
  REQUIRE_FALSE(audio_detail::decode_packet(ext.Format, nullptr, 1, AUDCLNT_BUFFERFLAGS_SILENT, 0, 10, StreamKind::SystemAudio).is_success());
}
TEST_CASE("Packed PCM24 sign extends and packets have bounded allocation") {
  WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, 48000, 144000, 3, 24, 0};
  const BYTE bytes[] = {0, 0, 0x80, 0, 0, 0x40};
  auto p = audio_detail::decode_packet(format, bytes, 2, 0, 0, 10, StreamKind::SystemAudio);
  REQUIRE(p.is_success());
  REQUIRE(p.value().interleaved == std::vector<float>{-1, 0.5f});
  REQUIRE_FALSE(audio_detail::decode_packet(format, nullptr, UINT32_MAX, AUDCLNT_BUFFERFLAGS_SILENT, 0, 10, StreamKind::SystemAudio).is_success());
}
TEST_CASE("Silent WASAPI packet permits null data and preserves native metadata") {
  WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0};
  auto result = audio_detail::decode_packet(format, nullptr, 480, AUDCLNT_BUFFERFLAGS_SILENT | AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, 10000000, 3000000, StreamKind::SystemAudio);
  REQUIRE(result.is_success());
  REQUIRE(result.value().pts == 3000000);
  REQUIRE(result.value().sample_rate == 48000);
  REQUIRE(result.value().channels == 2);
  REQUIRE(result.value().silent);
  REQUIRE(result.value().discontinuity);
  REQUIRE(result.value().interleaved == std::vector<float>(960, 0));
  REQUIRE_FALSE(audio_detail::decode_packet(format, nullptr, 1, 0, 0, 10, StreamKind::SystemAudio).is_success());
  REQUIRE_FALSE(audio_detail::decode_packet(format, nullptr, 1, AUDCLNT_BUFFERFLAGS_SILENT | AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR, 0, 10, StreamKind::SystemAudio).is_success());
}
TEST_CASE("PCM16 converts signed full scale and rejects invalid layout") {
  WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, 44100, 88200, 2, 16, 0};
  const short samples[] = {-32768, 0, 16384};
  auto result = audio_detail::decode_packet(format, reinterpret_cast<const BYTE*>(samples), 3, 0, 0, 10000000, StreamKind::MicrophoneAudio);
  REQUIRE(result.is_success());
  REQUIRE(result.value().interleaved == std::vector<float>{-1, 0, 0.5f});
  format.nBlockAlign = 1;
  REQUIRE_FALSE(audio_detail::decode_packet(format, reinterpret_cast<const BYTE*>(samples), 3, 0, 0, 10, StreamKind::MicrophoneAudio).is_success());
}
