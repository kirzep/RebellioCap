#pragma once
#include <Windows.h>
#include <optional>
#include <string_view>

namespace rebelliocap {
enum class GpuRecordingPriorityMode { Disabled, SoftRealtime, RelativeFallback };

constexpr std::string_view gpu_recording_priority_mode_name(GpuRecordingPriorityMode mode) noexcept {
  switch (mode) {
    case GpuRecordingPriorityMode::SoftRealtime: return "soft_realtime";
    case GpuRecordingPriorityMode::RelativeFallback: return "relative_fallback";
    default: return "disabled";
  }
}

struct GpuRecordingPriority {
  bool requested{false}, applied{false};
  int effective{0};
  bool effective_available{true};
  GpuRecordingPriorityMode mode{GpuRecordingPriorityMode::Disabled};
  std::optional<long> hresult;
};

// Lowest absolute soft-realtime level: preempt ordinary graphics work, while
// periodically yielding to it. This is device-scoped, never hard realtime30,
// CPU realtime, or a process scheduling-class change. No elevation is requested.
// DXGI's getter only documents relative [-7,7] values (and returned0 in actual
// absolute-mode probes), so setter acceptance must not invent an effective level.
template<class SetPriority, class GetPriority>
GpuRecordingPriority configure_recording_priority(bool requested,
    SetPriority set_priority, GetPriority get_priority) {
  GpuRecordingPriority result;
  result.requested=requested;
  if(!requested) return result;
  result.effective_available=false;
  constexpr int kLowestSoftRealtime = 0x40000010;
  const HRESULT absolute=set_priority(kLowestSoftRealtime);
  if(SUCCEEDED(absolute)) {
    result.mode=GpuRecordingPriorityMode::SoftRealtime;
    result.applied=true; // The documented request was accepted, not read back.
    return result;
  }
  // Retain the failure of the desired policy, even if a weaker request works.
  // Ordinary status then warns that the requested recording priority is absent.
  result.hresult=static_cast<long>(absolute);
  result.mode=GpuRecordingPriorityMode::RelativeFallback;
  if(FAILED(set_priority(7))) return result;
  int relative=0;
  if(SUCCEEDED(get_priority(&relative))) {
    result.effective=relative;
    result.effective_available=true;
  }
  return result;
}
} // namespace rebelliocap
