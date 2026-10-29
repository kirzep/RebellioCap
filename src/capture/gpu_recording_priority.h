#pragma once
#include <Windows.h>
#include <optional>

namespace rebelliocap {
struct GpuRecordingPriority {
  bool requested{false}, applied{false};
  int effective{0};
  std::optional<long> hresult;
};

// Relative DXGI priority, never absolute/realtime scheduling. Failure is a
// recording diagnostic, not a reason to stop capture or request elevation.
template<class SetPriority, class GetPriority>
GpuRecordingPriority configure_recording_priority(bool requested,
    SetPriority set_priority, GetPriority get_priority) {
  GpuRecordingPriority result;
  result.requested=requested;
  if(!requested) return result;
  const HRESULT set=set_priority(7);
  if(FAILED(set)) { result.hresult=static_cast<long>(set); return result; }
  const HRESULT read=get_priority(&result.effective);
  if(FAILED(read)) { result.hresult=static_cast<long>(read); return result; }
  result.applied=result.effective==7;
  return result;
}
} // namespace rebelliocap
