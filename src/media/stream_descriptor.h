#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "media/encoded_packet.h"

namespace rebelliocap {

inline const char* stream_title(StreamKind kind) {
  switch (kind) {
    case StreamKind::Video: return "Video";
    case StreamKind::MixedAudio: return "Системный звук + микрофон";
    case StreamKind::SystemAudio: return "Системный звук";
    case StreamKind::MicrophoneAudio: return "Микрофон";
  }
  return "Unknown";
}

struct Rational {
  std::int32_t numerator;
  std::int32_t denominator;
};

struct StreamDescriptor {
  StreamKind kind;
  std::string codec;
  std::string title;
  std::vector<std::byte> codec_extradata;
  Rational time_base;
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t sample_rate{0};
  std::uint32_t channels{0};
};

}  // namespace rebelliocap
