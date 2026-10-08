#pragma once
#include "audio/audio_capture_source.h"
#include "media/stream_descriptor.h"
namespace rebelliocap {
class IAudioEncoder {
 public:
 virtual ~IAudioEncoder()=default;
 virtual Result<std::vector<EncodedPacket>> encode(const PcmBlock&)=0;
 virtual Result<std::vector<EncodedPacket>> flush()=0;
 virtual const StreamDescriptor& descriptor() const noexcept=0;
};
}
