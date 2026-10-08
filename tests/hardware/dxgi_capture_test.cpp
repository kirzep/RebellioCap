#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "capture/d3d11_device.h"
#include "capture/dxgi_capture_source.h"
#include "capture/monitor_catalog.h"
#include "core/qpc_clock.h"
#include "platform/windows/com_apartment.h"

namespace {

constexpr int kSkipped = 77;

std::uint64_t luid_value(LUID luid) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32U) |
         static_cast<std::uint32_t>(luid.LowPart);
}

int fail(const std::string& reason) {
  std::cerr << "FAIL: " << reason << '\n';
  return 1;
}

bool is_desktop_unavailable(const rebelliocap::Error& error) {
  return error.code == "dxgi_capture.desktop_access_denied" ||
         error.code == "dxgi_capture.session_disconnected";
}

}  // namespace

int main() {
  using namespace std::chrono_literals;
  using namespace rebelliocap;

  const ComApartment apartment;
  if (!apartment.initialized() && apartment.result() != RPC_E_CHANGED_MODE) {
    return fail("COM initialization failed");
  }

  const auto access_lost = classify_dxgi_capture_failure(DXGI_ERROR_ACCESS_LOST);
  if (access_lost != CaptureFailureClass::RecoverableAccessLost) {
    return fail("DXGI_ERROR_ACCESS_LOST was not classified as recoverable");
  }
  if (classify_dxgi_capture_failure(DXGI_ERROR_UNSUPPORTED) !=
          CaptureFailureClass::Unsupported ||
      dxgi_capture_error("duplicate_output_failed", DXGI_ERROR_UNSUPPORTED).code !=
          "dxgi_capture.unsupported") {
    return fail("DXGI_ERROR_UNSUPPORTED was not classified as actionable incompatibility");
  }
  if (classify_dxgi_capture_failure(DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) !=
          CaptureFailureClass::DuplicationSessionLimit ||
      dxgi_capture_error("duplicate_output_failed", DXGI_ERROR_NOT_CURRENTLY_AVAILABLE).code !=
          "dxgi_capture.session_limit") {
    return fail("DXGI duplication session exhaustion was allowed to become a skip");
  }
  if (classify_dxgi_capture_failure(E_ACCESSDENIED) !=
          CaptureFailureClass::DesktopAccessDenied ||
      dxgi_capture_error("duplicate_output_failed", E_ACCESSDENIED).code !=
          "dxgi_capture.desktop_access_denied") {
    return fail("desktop access denial does not have an exact skippable classification");
  }
  if (classify_dxgi_capture_failure(DXGI_ERROR_SESSION_DISCONNECTED) !=
          CaptureFailureClass::SessionDisconnected ||
      dxgi_capture_error("duplicate_output_failed", DXGI_ERROR_SESSION_DISCONNECTED).code !=
          "dxgi_capture.session_disconnected") {
    return fail("session disconnection does not have an exact skippable classification");
  }

  auto monitors_result = enumerate_monitors();
  if (!monitors_result.is_success()) {
    return fail("monitor enumeration failed: " + monitors_result.error().code);
  }
  const auto& monitors = monitors_result.value();
  if (monitors.empty()) {
    std::cerr << "SKIP: no output is attached to an interactive desktop\n";
    return kSkipped;
  }

  const auto& monitor = monitors.front();
  auto device_result = create_d3d11_device(monitor.id.adapter_luid);
  if (!device_result.is_success()) {
    return fail("D3D11 device creation failed: " + device_result.error().code);
  }
  auto device = std::move(device_result).value();
  if (device.adapter_luid().HighPart != monitor.id.adapter_luid.HighPart ||
      device.adapter_luid().LowPart != monitor.id.adapter_luid.LowPart) {
    return fail("D3D11 device was created on the wrong adapter LUID");
  }

  QpcClock clock;
  auto source_result = DxgiCaptureSource::create(device, monitor.id, clock);
  if (!source_result.is_success()) {
    if (is_desktop_unavailable(source_result.error())) {
      std::cerr << "SKIP: " << source_result.error().message;
      if (source_result.error().hresult.has_value()) {
        std::cerr << " HRESULT=" << source_result.error().hresult.value();
      }
      std::cerr << '\n';
      return kSkipped;
    }
    return fail("desktop duplication creation failed: " + source_result.error().code);
  }
  auto source = std::move(source_result).value();
  const DXGI_OUTDUPL_DESC duplication_desc = source->description();

  const auto desktop_width =
      static_cast<std::uint32_t>(monitor.desktop_rect.right - monitor.desktop_rect.left);
  const auto desktop_height =
      static_cast<std::uint32_t>(monitor.desktop_rect.bottom - monitor.desktop_rect.top);
  const bool quarter_turn = monitor.rotation == DXGI_MODE_ROTATION_ROTATE90 ||
                            monitor.rotation == DXGI_MODE_ROTATION_ROTATE270;
  const auto expected_mode_width = quarter_turn ? desktop_height : desktop_width;
  const auto expected_mode_height = quarter_turn ? desktop_width : desktop_height;
  if (duplication_desc.Rotation != monitor.rotation ||
      duplication_desc.ModeDesc.Width != expected_mode_width ||
      duplication_desc.ModeDesc.Height != expected_mode_height) {
    return fail("actual duplication mode disagrees with monitor rotation accounting");
  }
  if (duplication_desc.ModeDesc.RefreshRate.Denominator == 0) {
    return fail("actual duplication mode reported an invalid refresh-rate denominator");
  }

  const auto deadline = std::chrono::steady_clock::now() + 60s;
  QpcTicks previous_timestamp = 0;
  std::uint64_t frame_count = 0;
  std::uint64_t timeout_count = 0;
  D3D11_TEXTURE2D_DESC acquired_desc{};

  while (std::chrono::steady_clock::now() < deadline) {
    auto frame_result = source->next_frame(100ms);
    if (!frame_result.is_success()) {
      if (is_desktop_unavailable(frame_result.error())) {
        std::cerr << "SKIP: " << frame_result.error().message;
        if (frame_result.error().hresult.has_value()) {
          std::cerr << " HRESULT=" << frame_result.error().hresult.value();
        }
        std::cerr << '\n';
        return kSkipped;
      }
      return fail("frame acquisition failed: " + frame_result.error().code);
    }
    if (!frame_result.value().has_value()) {
      ++timeout_count;
      continue;
    }

    auto frame = std::move(frame_result.value().value());
    if (frame.texture == nullptr || frame.lifetime == nullptr) {
      return fail("captured frame did not retain its GPU resource lease");
    }
    if (frame_count == 0) {
      auto overlapping_acquisition = source->next_frame(0ms);
      if (overlapping_acquisition.is_success() ||
          overlapping_acquisition.error().code != "dxgi_capture.frame_in_use") {
        return fail("capture allowed AcquireNextFrame while the prior lease was alive");
      }
    }
    if (frame.captured_at <= 0 || frame.captured_at <= previous_timestamp) {
      return fail("captured QPC timestamps are zero or non-monotonic");
    }
    previous_timestamp = frame.captured_at;

    frame.texture->GetDesc(&acquired_desc);
    if (acquired_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
      return fail("desktop texture is not BGRA8");
    }
    if (acquired_desc.Usage != D3D11_USAGE_DEFAULT || acquired_desc.CPUAccessFlags != 0) {
      return fail("desktop texture is CPU-readable or staging-backed");
    }
    if (frame.width != acquired_desc.Width || frame.height != acquired_desc.Height) {
      return fail("reported frame dimensions do not match the acquired texture");
    }
    if (frame.width != duplication_desc.ModeDesc.Width ||
        frame.height != duplication_desc.ModeDesc.Height ||
        acquired_desc.Format != duplication_desc.ModeDesc.Format) {
      return fail("acquired texture disagrees with the actual duplication mode");
    }

    ++frame_count;
  }

  if (frame_count == 0) {
    return fail("no real desktop frame was acquired during the 60-second test");
  }

  nlohmann::json evidence = {
      {"schema", 1},
      {"type", "dxgi_capture_evidence"},
      {"adapter_luid", luid_value(monitor.id.adapter_luid)},
      {"output_index", monitor.id.output_index},
      {"primary", monitor.primary},
      {"desktop_width", monitor.desktop_rect.right - monitor.desktop_rect.left},
      {"desktop_height", monitor.desktop_rect.bottom - monitor.desktop_rect.top},
      {"rotation", static_cast<std::uint32_t>(duplication_desc.Rotation)},
      {"mode_width", duplication_desc.ModeDesc.Width},
      {"mode_height", duplication_desc.ModeDesc.Height},
      {"mode_refresh_numerator", duplication_desc.ModeDesc.RefreshRate.Numerator},
      {"mode_refresh_denominator", duplication_desc.ModeDesc.RefreshRate.Denominator},
      {"mode_format", static_cast<std::uint32_t>(duplication_desc.ModeDesc.Format)},
      {"mode_scanline_ordering",
       static_cast<std::uint32_t>(duplication_desc.ModeDesc.ScanlineOrdering)},
      {"mode_scaling", static_cast<std::uint32_t>(duplication_desc.ModeDesc.Scaling)},
      {"texture_width", acquired_desc.Width},
      {"texture_height", acquired_desc.Height},
      {"texture_format", static_cast<std::uint32_t>(acquired_desc.Format)},
      {"gpu_only", acquired_desc.Usage == D3D11_USAGE_DEFAULT &&
                       acquired_desc.CPUAccessFlags == 0},
      {"frames", frame_count},
      {"timeouts", timeout_count},
      {"last_qpc", previous_timestamp},
      {"duration_seconds", 60}};
  std::cout << evidence.dump() << '\n';
  return 0;
}
