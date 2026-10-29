#include "capture/wgc_capture_source.h"
#include "capture/game_window_catalog.h"
#include <Windows.h>
#include <roapi.h>
#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <array>
#include <thread>
namespace rebelliocap {
namespace {
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using WinrtDevice = winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using FrameResult = Result<std::optional<CapturedVideoFrame>>;
// Each caller thread owns its own initialization; source lifetime may span
// threads without uninitializing another thread's apartment.
HRESULT initialize_runtime() {
  struct Apartment {
    HRESULT result{RoInitialize(RO_INIT_MULTITHREADED)};
    ~Apartment() { if (SUCCEEDED(result)) RoUninitialize(); }
  };
  thread_local Apartment apartment;
  return apartment.result == RPC_E_CHANGED_MODE ? S_OK : apartment.result;
}
Error capture_error(const char* code, const char* message, HRESULT hr) { return {code, message, static_cast<long>(hr)}; }
}  // namespace
struct WgcCaptureSource::Impl {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  std::filesystem::path executable;
  QpcClock& clock;
  std::optional<GameWindowIdentity> target;
  WinrtDevice runtime_device{nullptr};
  GraphicsCaptureItem item{nullptr};
  Direct3D11CaptureFramePool pool{nullptr};
  GraphicsCaptureSession session{nullptr};
  SizeInt32 pool_size{};
  // Exactly two owned output slots, retained independently of the WGC pool.
  // A frame's lifetime pins its slot; source destruction leaves that slot alive.
  std::array<std::shared_ptr<Microsoft::WRL::ComPtr<ID3D11Texture2D>>, 2> output_slots;
  std::atomic<bool> waiting{true};
  bool was_minimized{false};
  std::chrono::steady_clock::time_point retry_after{};
  std::chrono::steady_clock::time_point last_frame{};
  Impl(const D3d11Device& source, std::filesystem::path path, QpcClock& qpc)
    : device(source.device()), context(source.context()), executable(std::move(path)), clock(qpc) {}
  ~Impl() { reset(); }
  void reset() noexcept {
    // No FrameArrived or Closed delegates are installed. Close cannot race a
    // callback accessing this object or the caller's immediate context.
    if (session) { try { session.Close(); } catch (...) {} session = nullptr; }
    if (pool) { try { pool.Close(); } catch (...) {} pool = nullptr; }
    item = nullptr; target.reset(); waiting.store(true, std::memory_order_relaxed);
  }
  bool valid() const {
    if (!target) return false;
    auto actual = inspect_game_window(target->window);
    return actual && game_window_identity_equal(*target, *actual) && game_executable_equal(actual->executable, executable);
  }
  bool start() {
    if (std::chrono::steady_clock::now() < retry_after) return false;
    retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto candidate = find_game_window(executable);
    if (!candidate) return false;
    target = std::move(candidate);
    auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(factory->CreateForWindow(target->window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
    // Check again after item creation: no replacement HWND may inherit it.
    if (!valid()) { reset(); return false; }
    pool_size = item.Size();
    if (pool_size.Width <= 0 || pool_size.Height <= 0) { reset(); return false; }
    pool = Direct3D11CaptureFramePool::CreateFreeThreaded(runtime_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, pool_size);
    session = pool.CreateCaptureSession(item);
    // A system cursor can be above an unrelated foreground window. Do not
    // include it in the isolated game image. Unsupported old APIs fail closed.
    session.IsCursorCaptureEnabled(false);
    if (!valid()) { reset(); return false; }
    session.StartCapture();
    was_minimized=IsIconic(target->window)!=FALSE;
    last_frame = std::chrono::steady_clock::now();
    return true;
  }
  FrameResult poll() {
    const HRESULT removed = device->GetDeviceRemovedReason();
    if (FAILED(removed)) return FrameResult::failure(capture_error("wgc_capture.device_removed", "The capture device is unavailable.", removed));
    if (target && !valid()) reset();
    if (target) {
      const bool minimized=IsIconic(target->window)!=FALSE;
      if(was_minimized && !minimized) {
        // Windows can leave the old pool latched to the minimized surface.
        // Reopen only this pinned executable after the restored compositor
        // surface has had a short opportunity to settle.
        reset(); retry_after=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
      }
      was_minimized=minimized;
    }
    if (!pool && !start()) return FrameResult::success(std::nullopt);
    auto frame = pool.TryGetNextFrame();
    if (!frame) {
      if (IsIconic(target->window) || std::chrono::steady_clock::now() - last_frame > std::chrono::milliseconds(500)) waiting.store(true, std::memory_order_relaxed);
      return FrameResult::success(std::nullopt);
    }
    // Read the newest available image, with a bounded drain. A checked-out
    // frame never escapes into the encoder or survives pool recreation.
    if (auto newer = pool.TryGetNextFrame()) { frame.Close(); frame = std::move(newer); }
    const auto size = frame.ContentSize();
    if (size.Width <= 0 || size.Height <= 0 || !valid()) {
      frame.Close(); reset(); return FrameResult::success(std::nullopt);
    }
    // Keep a minimized session alive: a game/Windows version may still deliver
    // real frames. Discard a collapsed thumbnail size rather than replacing the
    // last full game image. No synthetic screen pixels enter the source.
    if (IsIconic(target->window) && (size.Width != pool_size.Width || size.Height != pool_size.Height)) {
      frame.Close(); waiting.store(true, std::memory_order_relaxed);
      return FrameResult::success(std::nullopt);
    }
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
    D3D11_TEXTURE2D_DESC description{}; texture->GetDesc(&description);
    if (static_cast<UINT>(size.Width) > description.Width || static_cast<UINT>(size.Height) > description.Height) {
      // Growing content is clipped in the old pool. Discard that image and
      // recreate rather than expose missing or undefined texture regions.
      frame.Close(); pool_size = size;
      pool.Recreate(runtime_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, pool_size);
      waiting.store(true, std::memory_order_relaxed);
      return FrameResult::success(std::nullopt);
    }
    D3D11_TEXTURE2D_DESC owned_description{};
    owned_description.Width = static_cast<UINT>(size.Width); owned_description.Height = static_cast<UINT>(size.Height);
    owned_description.MipLevels = 1; owned_description.ArraySize = 1;
    owned_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; owned_description.SampleDesc.Count = 1;
    owned_description.Usage = D3D11_USAGE_DEFAULT; owned_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::shared_ptr<Microsoft::WRL::ComPtr<ID3D11Texture2D>> lease;
    for (auto& slot : output_slots) {
      if (slot && slot.use_count() != 1) continue;
      if (!slot) slot = std::make_shared<Microsoft::WRL::ComPtr<ID3D11Texture2D>>();
      D3D11_TEXTURE2D_DESC existing{};
      if (*slot) (*slot)->GetDesc(&existing);
      if (existing.Width != owned_description.Width || existing.Height != owned_description.Height) {
        // Release this unused slot before allocating its replacement, so resize
        // cannot grow retained GPU memory beyond the two output slots.
        slot->Reset();
        winrt::check_hresult(device->CreateTexture2D(&owned_description, nullptr, slot->GetAddressOf()));
      }
      lease = slot;
      break;
    }
    if (!lease) { frame.Close(); return FrameResult::success(std::nullopt); }
    const D3D11_BOX content{0, 0, 0, owned_description.Width, owned_description.Height, 1};
    // Only the capture thread uses this immediate context, matching converter
    // ownership. Crop smaller ContentSize; remainder of WGC surface is undefined.
    context->CopySubresourceRegion(lease->Get(), 0, 0, 0, 0, texture.Get(), 0, &content);
    const auto time = frame.SystemRelativeTime().count();
    const QpcTicks timestamp = (time / 10000000) * clock.frequency() + (time % 10000000) * clock.frequency() / 10000000;
    frame.Close();
    if (!valid()) { reset(); return FrameResult::success(std::nullopt); }
    if (size.Width != pool_size.Width || size.Height != pool_size.Height) {
      pool_size = size;
      pool.Recreate(runtime_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, pool_size);
    }
    waiting.store(false, std::memory_order_relaxed); last_frame = std::chrono::steady_clock::now();
    return FrameResult::success(CapturedVideoFrame{*lease, timestamp, owned_description.Width, owned_description.Height, std::move(lease)});
  }
};
WgcCaptureSource::WgcCaptureSource(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
WgcCaptureSource::~WgcCaptureSource() = default;
Result<std::unique_ptr<WgcCaptureSource>> WgcCaptureSource::create(const D3d11Device& device, std::filesystem::path executable, QpcClock& clock) {
  using SourceResult = Result<std::unique_ptr<WgcCaptureSource>>;
  if (!device.device() || !device.context() || executable.empty() || !executable.is_absolute())
    return SourceResult::failure({"wgc_capture.invalid_target", "Select the game's full executable path and a valid capture device.", {}});
  auto initialized = initialize_runtime();
  if (FAILED(initialized)) return SourceResult::failure(capture_error("wgc_capture.runtime_failed", "Could not initialize Windows capture.", initialized));
  try {
    if (!GraphicsCaptureSession::IsSupported()) return SourceResult::failure({"wgc_capture.unsupported", "Windows Graphics Capture is unavailable.", {}});
    auto impl = std::make_unique<Impl>(device, std::move(executable), clock);
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    winrt::check_hresult(device.device()->QueryInterface(IID_PPV_ARGS(&dxgi)));
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
    impl->runtime_device = inspectable.as<WinrtDevice>();
    return SourceResult::success(std::unique_ptr<WgcCaptureSource>(new WgcCaptureSource(std::move(impl))));
  } catch (const winrt::hresult_error& error) {
    return SourceResult::failure(capture_error("wgc_capture.initialization_failed", "Could not initialize selected-window capture.", error.code()));
  }
}
bool WgcCaptureSource::waiting_for_game() const noexcept { return impl_->waiting.load(std::memory_order_relaxed); }
Result<std::optional<CapturedVideoFrame>> WgcCaptureSource::next_frame(std::chrono::milliseconds timeout) {
  const auto initialized = initialize_runtime();
  if (FAILED(initialized)) return FrameResult::failure(capture_error("wgc_capture.runtime_failed", "Could not initialize Windows capture.", initialized));
  const auto deadline = std::chrono::steady_clock::now() + (std::max)(timeout, std::chrono::milliseconds::zero());
  do {
    try {
      auto result = impl_->poll();
      if (!result.is_success() || result.value()) return result;
    } catch (const winrt::hresult_error&) {
      impl_->reset();
      impl_->retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      // Revoked/closed/denied window sessions are expected source waiting. GPU
      // removal is surfaced on the next poll; never substitute another source.
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    std::this_thread::sleep_for((std::min)(std::chrono::milliseconds(2), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
  } while (std::chrono::steady_clock::now() < deadline);
  return FrameResult::success(std::nullopt);
}
}  // namespace rebelliocap
