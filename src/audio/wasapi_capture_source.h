#pragma once
#include <memory>
#include <string>
#include "audio/audio_capture_source.h"

namespace rebelliocap {
enum class AudioEndpointRole { SystemLoopback, Microphone };
struct AudioEndpointSelection {
  AudioEndpointRole role;
  // Required by create(); no implicit-default or endpoint-loss fallback.
  std::optional<std::wstring> endpoint_id;
};
// Create, use, and destroy on one COM-owning capture worker. No internal thread.
class WasapiCaptureSource final : public IAudioCaptureSource {
 public:
  static Result<std::unique_ptr<WasapiCaptureSource>> create(AudioEndpointSelection selection, QpcClock& clock);
  ~WasapiCaptureSource() override;
  Result<std::optional<PcmBlock>> next_block(std::chrono::milliseconds timeout) override;
 private:
  struct Impl;
  explicit WasapiCaptureSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}
