#include "audio/wasapi_pcm.h"
#include <audioclient.h>
#include <ksmedia.h>
#include <cstring>
#include <limits>
namespace rebelliocap::audio_detail {
Result<QpcTicks> qpc_from_100ns(std::uint64_t value, QpcTicks frequency) {
  constexpr std::uint64_t scale = 10000000;
  const auto fail = [] { return Result<QpcTicks>::failure({"audio.timestamp_invalid", "WASAPI timestamp exceeds signed QPC range", {}}); };
  if (frequency <= 0) return fail();
  const auto f = static_cast<std::uint64_t>(frequency);
  const auto seconds = value / scale, remainder = value % scale;
  const auto limit = static_cast<std::uint64_t>((std::numeric_limits<QpcTicks>::max)());
  if (seconds > limit / f) return fail();
  // Split both factors: neither remainder product can overflow uint64.
  const auto fraction = remainder * (f / scale) + remainder * (f % scale) / scale;
  const auto whole = seconds * f;
  if (fraction > limit - whole) return fail();
  return Result<QpcTicks>::success(static_cast<QpcTicks>(whole + fraction));
}
Result<PcmBlock> decode_packet(const WAVEFORMATEX& format, const BYTE* data, std::uint32_t frames, DWORD flags, std::uint64_t timestamp, QpcTicks frequency, StreamKind stream) {
  auto fail = [](const char* code) { return Result<PcmBlock>::failure({code, "Invalid WASAPI PCM packet", {}}); };
  // Windows marks this packet's timing as uncertain during stream transitions.
  // Do not feed its timestamps into AAC; let the owning worker reopen the stream.
  if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
    return Result<PcmBlock>::failure({"audio.timestamp_unreliable",
        "WASAPI reported an uncertain packet timestamp; reopen the pinned stream", {}});
  auto pts = qpc_from_100ns(timestamp, frequency);
  if (!pts.is_success()) return Result<PcmBlock>::failure(pts.error());
  WORD tag = format.wFormatTag;
  DWORD mask = 0;
  WORD valid_bits = format.wBitsPerSample;
  if (tag == WAVE_FORMAT_EXTENSIBLE) {
    if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return fail("audio.format_unsupported");
    const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
    mask = ext.dwChannelMask;
    valid_bits = ext.Samples.wValidBitsPerSample;
    if (ext.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
    else if (ext.SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
    else return fail("audio.format_unsupported");
  }
  const bool floating = tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32;
  const bool integer = tag == WAVE_FORMAT_PCM && (format.wBitsPerSample == 16 || format.wBitsPerSample == 24 || format.wBitsPerSample == 32);
  if ((!floating && !integer) || !valid_bits || valid_bits > format.wBitsPerSample || !format.nChannels || format.nChannels > 32 || !format.nSamplesPerSec || format.nSamplesPerSec > 768000 || frames > format.nSamplesPerSec || format.nBlockAlign != format.nChannels * (format.wBitsPerSample / 8)) return fail("audio.format_unsupported");
  const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
  if (!silent && frames && !data) return fail("audio.packet_invalid");
  PcmBlock block{stream, pts.value(), format.nSamplesPerSec, format.nChannels, std::vector<float>(static_cast<size_t>(frames) * format.nChannels, 0), mask, silent, (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0};
  if (silent) return Result<PcmBlock>::success(std::move(block));
  const auto bytes = format.wBitsPerSample / 8;
  for (size_t i = 0; i < block.interleaved.size(); ++i) {
    const BYTE* sample = data + i * bytes;
    if (floating) std::memcpy(&block.interleaved[i], sample, sizeof(float));
    else {
      // PCM extensible valid bits are left-aligned in their container.
      std::int32_t value = 0;
      if (bytes == 2) { std::int16_t v; std::memcpy(&v, sample, 2); value = static_cast<std::int32_t>(v) * 65536; }
      else if (bytes == 3) { const auto u = (static_cast<std::uint32_t>(sample[0]) << 8) | (static_cast<std::uint32_t>(sample[1]) << 16) | (static_cast<std::uint32_t>(sample[2]) << 24); std::memcpy(&value, &u, 4); }
      else std::memcpy(&value, sample, 4);
      block.interleaved[i] = static_cast<float>(static_cast<double>(value) / 2147483648.0);
    }
  }
  return Result<PcmBlock>::success(std::move(block));
}
}
