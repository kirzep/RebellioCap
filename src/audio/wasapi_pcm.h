#pragma once
#include <windows.h>
#include <mmreg.h>
#include "audio/audio_capture_source.h"
namespace rebelliocap::audio_detail {
Result<QpcTicks> qpc_from_100ns(std::uint64_t value, QpcTicks frequency);
Result<PcmBlock> decode_packet(const WAVEFORMATEX& format, const BYTE* data, std::uint32_t frames, DWORD flags, std::uint64_t timestamp, QpcTicks frequency, StreamKind stream);
}
