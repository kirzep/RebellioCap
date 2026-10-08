#include "capture/dxgi_capture_source.h"

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <new>
#include <mutex>
#include <string>
#include <utility>

namespace rebelliocap {
namespace {

using Microsoft::WRL::ComPtr;

bool luid_equal(LUID left, LUID right) noexcept {
  return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

}  // namespace

Error dxgi_capture_error(std::string operation, HRESULT result) {
  switch (classify_dxgi_capture_failure(result)) {
    case CaptureFailureClass::RecoverableAccessLost:
      return Error{"dxgi_capture.access_lost",
                   "Desktop duplication access was lost and the source must be recreated.",
                   static_cast<long>(result)};
    case CaptureFailureClass::DesktopAccessDenied:
      return Error{"dxgi_capture.desktop_access_denied",
                   "Windows denied access to the current secure or non-interactive desktop.",
                   static_cast<long>(result)};
    case CaptureFailureClass::SessionDisconnected:
      return Error{"dxgi_capture.session_disconnected",
                   "The interactive Windows session disconnected during desktop duplication.",
                   static_cast<long>(result)};
    case CaptureFailureClass::Unsupported:
      return Error{"dxgi_capture.unsupported",
                   "Desktop Duplication is unsupported for the selected output or graphics mode.",
                   static_cast<long>(result)};
    case CaptureFailureClass::DuplicationSessionLimit:
      return Error{"dxgi_capture.session_limit",
                   "Windows has no free Desktop Duplication session for this desktop.",
                   static_cast<long>(result)};
    case CaptureFailureClass::Fatal:
      return Error{"dxgi_capture." + std::move(operation),
                   "Desktop duplication failed with an unrecoverable error.",
                   static_cast<long>(result)};
  }
  return Error{"dxgi_capture.unknown", "Desktop duplication failed.",
               static_cast<long>(result)};
}

namespace {

DWORD timeout_milliseconds(std::chrono::milliseconds timeout) noexcept {
  if (timeout.count() <= 0) {
    return 0;
  }
  constexpr auto maximum =
      static_cast<std::int64_t>((std::numeric_limits<DWORD>::max)() - 1U);
  return static_cast<DWORD>((std::min)(timeout.count(), maximum));
}

}  // namespace

struct DxgiCaptureSource::SharedDuplicationState {
  ~SharedDuplicationState() {
    // A caller lease keeps this state (including the device and duplication
    // interface) alive after the source is destroyed. Release only when its
    // final owner is gone, before releasing the COM interfaces themselves.
    if (frame_held && duplication != nullptr) {
      static_cast<void>(duplication->ReleaseFrame());
    }
  }

  ComPtr<ID3D11Device> device;
  ComPtr<IDXGIOutputDuplication> duplication;
  DXGI_OUTDUPL_DESC description{};
  std::mutex mutex;
  bool frame_held{false};
  bool frame_leased{false};
  bool image_delivered{false};
  HRESULT last_release_result{S_OK};
};

class DuplicationFrameLease final {
 public:
  explicit DuplicationFrameLease(
      std::shared_ptr<DxgiCaptureSource::SharedDuplicationState> state)
      : state_(std::move(state)) {}

  ~DuplicationFrameLease() {
    std::lock_guard lock(state_->mutex);
    state_->frame_leased = false;
  }

  DuplicationFrameLease(const DuplicationFrameLease&) = delete;
  DuplicationFrameLease& operator=(const DuplicationFrameLease&) = delete;

 private:
  std::shared_ptr<DxgiCaptureSource::SharedDuplicationState> state_;
};

CaptureFailureClass classify_dxgi_capture_failure(HRESULT result) noexcept {
  if (result == DXGI_ERROR_ACCESS_LOST) {
    return CaptureFailureClass::RecoverableAccessLost;
  }
  if (result == E_ACCESSDENIED) {
    return CaptureFailureClass::DesktopAccessDenied;
  }
  if (result == DXGI_ERROR_SESSION_DISCONNECTED) {
    return CaptureFailureClass::SessionDisconnected;
  }
  if (result == DXGI_ERROR_UNSUPPORTED) {
    return CaptureFailureClass::Unsupported;
  }
  if (result == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
    return CaptureFailureClass::DuplicationSessionLimit;
  }
  return CaptureFailureClass::Fatal;
}

DxgiCaptureSource::DxgiCaptureSource(std::shared_ptr<SharedDuplicationState> state,
                                     QpcClock& clock)
    : state_(std::move(state)), clock_(clock) {}

DxgiCaptureSource::~DxgiCaptureSource() = default;

DXGI_OUTDUPL_DESC DxgiCaptureSource::description() const noexcept {
  return state_->description;
}

Result<std::unique_ptr<DxgiCaptureSource>> DxgiCaptureSource::create(
    const D3d11Device& device, MonitorId monitor, QpcClock& clock) {
  if (!luid_equal(device.adapter_luid(), monitor.adapter_luid)) {
    return Result<std::unique_ptr<DxgiCaptureSource>>::failure(Error{
        "dxgi_capture.adapter_mismatch",
        "The D3D11 device and selected monitor belong to different adapters.", std::nullopt});
  }

  ComPtr<IDXGIFactory1> factory;
  HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(result)) {
    return Result<std::unique_ptr<DxgiCaptureSource>>::failure(
        dxgi_capture_error("factory_failed", result));
  }

  ComPtr<IDXGIAdapter1> selected_adapter;
  for (UINT adapter_index = 0;; ++adapter_index) {
    ComPtr<IDXGIAdapter1> adapter;
    result = factory->EnumAdapters1(adapter_index, &adapter);
    if (result == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(result)) {
      return Result<std::unique_ptr<DxgiCaptureSource>>::failure(
          dxgi_capture_error("adapter_enumeration_failed", result));
    }
    DXGI_ADAPTER_DESC1 description{};
    result = adapter->GetDesc1(&description);
    if (FAILED(result)) {
      return Result<std::unique_ptr<DxgiCaptureSource>>::failure(
          dxgi_capture_error("adapter_description_failed", result));
    }
    if (luid_equal(description.AdapterLuid, monitor.adapter_luid)) {
      selected_adapter = std::move(adapter);
      break;
    }
  }
  if (selected_adapter == nullptr) {
    return Result<std::unique_ptr<DxgiCaptureSource>>::failure(Error{
        "dxgi_capture.adapter_not_found", "The selected monitor adapter is unavailable.",
        std::nullopt});
  }

  ComPtr<IDXGIOutput> output;
  result = selected_adapter->EnumOutputs(monitor.output_index, &output);
  if (FAILED(result)) {
    return Result<std::unique_ptr<DxgiCaptureSource>>::failure(
        dxgi_capture_error("output_not_found", result));
  }
  DXGI_OUTPUT_DESC output_description{};
  result = output->GetDesc(&output_description);
  if (FAILED(result)) {
    return Result<std::unique_ptr<DxgiCaptureSource>>::failure(
        dxgi_capture_error("output_description_failed", result));
  }
  if (output_description.AttachedToDesktop == FALSE) {
    return Result<std::unique_ptr<DxgiCaptureSource>>::failure(Error{
        "dxgi_capture.output_not_attached", "The selected output is not attached to the desktop.",
        std::nullopt});
  }

  ComPtr<IDXGIOutput1> output1;
  result = output.As(&output1);
  if (FAILED(result)) {
    return Result<std::unique_ptr<DxgiCaptureSource>>::failure(
        dxgi_capture_error("output1_unavailable", result));
  }

  auto state = std::make_shared<SharedDuplicationState>();
  state->device = device.device();
  result = output1->DuplicateOutput(device.device(), &state->duplication);
  if (FAILED(result)) {
    return Result<std::unique_ptr<DxgiCaptureSource>>::failure(
        dxgi_capture_error("duplicate_output_failed", result));
  }
  state->duplication->GetDesc(&state->description);

  return Result<std::unique_ptr<DxgiCaptureSource>>::success(
      std::unique_ptr<DxgiCaptureSource>(new DxgiCaptureSource(std::move(state), clock)));
}

Result<std::optional<CapturedVideoFrame>> DxgiCaptureSource::next_frame(
    std::chrono::milliseconds timeout) {
  std::lock_guard lock(state_->mutex);
  if (state_->frame_leased) {
    return Result<std::optional<CapturedVideoFrame>>::failure(Error{
        "dxgi_capture.frame_in_use",
        "Release the previous CapturedVideoFrame lease before requesting another frame.",
        std::nullopt});
  }
  if (state_->frame_held) {
    // Keep ownership between recorder ticks so Windows accumulates dirty
    // regions instead of copying every game/display update to this surface.
    // Microsoft's Desktop Duplication contract recommends releasing directly
    // before acquiring the next frame to avoid that otherwise wasted GPU work.
    state_->last_release_result = state_->duplication->ReleaseFrame();
    state_->frame_held = false;
  }
  if (FAILED(state_->last_release_result)) {
    return Result<std::optional<CapturedVideoFrame>>::failure(
        dxgi_capture_error("release_frame_failed", state_->last_release_result));
  }

  DXGI_OUTDUPL_FRAME_INFO frame_info{};
  ComPtr<IDXGIResource> resource;
  const HRESULT result = state_->duplication->AcquireNextFrame(
      timeout_milliseconds(timeout), &frame_info, &resource);
  if (result == DXGI_ERROR_WAIT_TIMEOUT) {
    return Result<std::optional<CapturedVideoFrame>>::success(std::nullopt);
  }
  if (FAILED(result)) {
    return Result<std::optional<CapturedVideoFrame>>::failure(
        dxgi_capture_error("acquire_frame_failed", result));
  }

  state_->frame_held = true;
  // Pointer metadata alone does not change the desktop pixels. Keep ownership
  // until the next tick, while the pipeline reuses its cached desktop image.
  if (state_->image_delivered && frame_info.LastPresentTime.QuadPart == 0) {
    return Result<std::optional<CapturedVideoFrame>>::success(std::nullopt);
  }
  ComPtr<ID3D11Texture2D> texture;
  const HRESULT texture_result = resource.As(&texture);
  if (FAILED(texture_result)) {
    state_->last_release_result = state_->duplication->ReleaseFrame();
    state_->frame_held = false;
    return Result<std::optional<CapturedVideoFrame>>::failure(
        dxgi_capture_error("texture_query_failed", texture_result));
  }

  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
      description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0) {
    state_->last_release_result = state_->duplication->ReleaseFrame();
    state_->frame_held = false;
    return Result<std::optional<CapturedVideoFrame>>::failure(Error{
        "dxgi_capture.invalid_texture",
        "Desktop Duplication returned a texture that violates the BGRA GPU-only contract.",
        std::nullopt});
  }

  std::shared_ptr<DuplicationFrameLease> lease;
  try {
    lease = std::make_shared<DuplicationFrameLease>(state_);
  } catch (const std::bad_alloc&) {
    state_->last_release_result = state_->duplication->ReleaseFrame();
    state_->frame_held = false;
    return Result<std::optional<CapturedVideoFrame>>::failure(Error{
        "dxgi_capture.allocation_failed", "Could not allocate the frame lifetime lease.",
        std::nullopt});
  }
  state_->frame_leased = true;
  state_->image_delivered = true;

  QpcTicks captured_at = clock_.now();
  if (captured_at <= last_timestamp_) {
    captured_at = last_timestamp_ + 1;
  }
  last_timestamp_ = captured_at;

  CapturedVideoFrame frame{texture, captured_at, description.Width, description.Height,
                           std::move(lease), state_->description.Rotation};
  return Result<std::optional<CapturedVideoFrame>>::success(
      std::optional<CapturedVideoFrame>(std::move(frame)));
}

}  // namespace rebelliocap
