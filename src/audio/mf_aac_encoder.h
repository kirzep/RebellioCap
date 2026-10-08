#pragma once
#include "audio/audio_encoder.h"
namespace rebelliocap {
// Create, use and destroy on one COM thread. Feed contiguous normalized PCM.
// Gaps require a new encoder epoch; flush is idempotent end-of-stream.
class MfAacEncoder final:public IAudioEncoder {
 public:
 static Result<std::unique_ptr<MfAacEncoder>> create(StreamKind,std::uint32_t,QpcClock&,std::uint32_t epoch=0);
 ~MfAacEncoder();
 Result<std::vector<EncodedPacket>> encode(const PcmBlock&) override;
 Result<std::vector<EncodedPacket>> flush() override;
 const StreamDescriptor& descriptor() const noexcept override;
 private: struct Impl; explicit MfAacEncoder(std::unique_ptr<Impl>); std::unique_ptr<Impl> impl_;
};
}
