#include "capture/dxgi_capture_source.cpp"

#include <catch2/catch_test_macros.hpp>
#include <wrl/implements.h>

#include <memory>
#include <string>
#include <vector>

namespace rebelliocap {

struct DxgiCaptureSourceTestAccess {
  static std::unique_ptr<DxgiCaptureSource> create(
      Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication,
      ID3D11Device* device, QpcClock& clock) {
    auto state = std::make_shared<DxgiCaptureSource::SharedDuplicationState>();
    state->device = device;
    state->duplication = std::move(duplication);
    state->duplication->GetDesc(&state->description);
    return std::unique_ptr<DxgiCaptureSource>(
        new DxgiCaptureSource(std::move(state), clock));
  }
};

}  // namespace rebelliocap

namespace {

using namespace rebelliocap;
using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;

struct DriverTrace {
  std::vector<std::string> calls;
  bool frame_owned{false};
  int desktop_copies{0};
  int destroyed{0};
  bool destroyed_with_frame{false};

  // Desktop Duplication copies each desktop update only while the client does
  // not own a frame (the documented reason to defer ReleaseFrame until acquire).
  void desktop_update() {
    if (!frame_owned) ++desktop_copies;
  }
};

class DuplicationDriver final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IDXGIOutputDuplication> {
 public:
  DuplicationDriver(std::shared_ptr<DriverTrace> trace,
                    ComPtr<ID3D11Texture2D> texture)
      : trace_(std::move(trace)), texture_(std::move(texture)) {}

  ~DuplicationDriver() override {
    ++trace_->destroyed;
    trace_->destroyed_with_frame = trace_->frame_owned;
  }

  HRESULT acquire_result{S_OK};
  HRESULT release_result{S_OK};
  LONGLONG last_present_time{1};

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }

  void STDMETHODCALLTYPE GetDesc(DXGI_OUTDUPL_DESC* description) override {
    *description = {};
    description->ModeDesc.Width = description->ModeDesc.Height = 16;
    description->ModeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  }

  HRESULT STDMETHODCALLTYPE AcquireNextFrame(
      UINT, DXGI_OUTDUPL_FRAME_INFO* info, IDXGIResource** resource) override {
    trace_->calls.emplace_back("acquire");
    if (trace_->frame_owned) return DXGI_ERROR_INVALID_CALL;
    if (FAILED(acquire_result)) return acquire_result;
    const HRESULT result = texture_.CopyTo(IID_PPV_ARGS(resource));
    if (FAILED(result)) return result;
    *info = {};
    info->LastPresentTime.QuadPart = last_present_time;
    trace_->frame_owned = true;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetFrameDirtyRects(UINT, RECT*, UINT*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetFrameMoveRects(
      UINT, DXGI_OUTDUPL_MOVE_RECT*, UINT*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFramePointerShape(
      UINT, void*, UINT*, DXGI_OUTDUPL_POINTER_SHAPE_INFO*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE MapDesktopSurface(DXGI_MAPPED_RECT*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE UnMapDesktopSurface() override { return E_NOTIMPL; }

  HRESULT STDMETHODCALLTYPE ReleaseFrame() override {
    trace_->calls.emplace_back("release");
    if (!trace_->frame_owned) return DXGI_ERROR_INVALID_CALL;
    trace_->frame_owned = false;
    return release_result;
  }

 private:
  std::shared_ptr<DriverTrace> trace_;
  ComPtr<ID3D11Texture2D> texture_;
};

struct CaptureFixture {
  QpcClock clock;
  std::shared_ptr<DriverTrace> trace{std::make_shared<DriverTrace>()};
  DuplicationDriver* driver{nullptr};
  std::unique_ptr<DxgiCaptureSource> source;

  CaptureFixture() {
    ComPtr<ID3D11Device> device;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, nullptr)));
    D3D11_TEXTURE2D_DESC description{};
    description.Width = description.Height = 16;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &texture)));
    auto duplication = Microsoft::WRL::Make<DuplicationDriver>(trace, std::move(texture));
    REQUIRE(duplication != nullptr);
    driver = duplication.Get();
    source = DxgiCaptureSourceTestAccess::create(
        std::move(duplication), device.Get(), clock);
  }

  CapturedVideoFrame next() {
    auto result = source->next_frame(0ms);
    REQUIRE(result.is_success());
    REQUIRE(result.value().has_value());
    return std::move(*result.value());
  }
};

}  // namespace

TEST_CASE("Desktop updates between recorder ticks do not trigger duplication copies") {
  CaptureFixture fixture;
  auto frame = fixture.next();
  frame = {};
  fixture.trace->desktop_update();
  fixture.trace->desktop_update();
  fixture.trace->desktop_update();
  CHECK(fixture.trace->desktop_copies == 0);
  CHECK(fixture.trace->calls == std::vector<std::string>{"acquire"});

  auto next = fixture.next();
  CHECK(fixture.trace->calls ==
        std::vector<std::string>{"acquire", "release", "acquire"});
}

TEST_CASE("A copied caller lease prevents acquisition until its last owner drops") {
  CaptureFixture fixture;
  auto frame = fixture.next();
  auto retained = frame.lifetime;
  frame = {};
  auto overlapping = fixture.source->next_frame(0ms);
  REQUIRE_FALSE(overlapping.is_success());
  CHECK(overlapping.error().code == "dxgi_capture.frame_in_use");
  CHECK(fixture.trace->calls == std::vector<std::string>{"acquire"});
  retained.reset();
  auto next = fixture.next();
  CHECK(fixture.trace->calls ==
        std::vector<std::string>{"acquire", "release", "acquire"});
}

TEST_CASE("Destroying the source releases an idle retained duplication frame") {
  CaptureFixture fixture;
  auto frame = fixture.next();
  frame = {};
  CHECK(fixture.trace->calls == std::vector<std::string>{"acquire"});
  fixture.source.reset();
  CHECK(fixture.trace->calls == std::vector<std::string>{"acquire", "release"});
  CHECK(fixture.trace->destroyed == 1);
  CHECK_FALSE(fixture.trace->destroyed_with_frame);
}

TEST_CASE("Destroying the source preserves the driver and texture for a live caller") {
  CaptureFixture fixture;
  auto frame = fixture.next();
  fixture.source.reset();
  CHECK(fixture.trace->destroyed == 0);
  CHECK(fixture.trace->calls == std::vector<std::string>{"acquire"});
  D3D11_TEXTURE2D_DESC description{};
  frame.texture->GetDesc(&description);
  CHECK(description.Width == 16);
  frame = {};
  CHECK(fixture.trace->calls == std::vector<std::string>{"acquire", "release"});
  CHECK(fixture.trace->destroyed == 1);
  CHECK_FALSE(fixture.trace->destroyed_with_frame);
}

TEST_CASE("Access lost while releasing a retained frame stops acquisition and frees ownership") {
  CaptureFixture fixture;
  auto frame = fixture.next();
  frame = {};
  fixture.driver->release_result = DXGI_ERROR_ACCESS_LOST;
  auto result = fixture.source->next_frame(0ms);
  REQUIRE_FALSE(result.is_success());
  CHECK(result.error().code == "dxgi_capture.access_lost");
  CHECK(fixture.trace->calls == std::vector<std::string>{"acquire", "release"});
  fixture.source.reset();
  CHECK(fixture.trace->destroyed == 1);
  CHECK(fixture.trace->calls == std::vector<std::string>{"acquire", "release"});
}

TEST_CASE("Acquisition failure after a deferred release leaves no held frame") {
  CaptureFixture fixture;
  auto frame = fixture.next();
  frame = {};
  fixture.driver->acquire_result = DXGI_ERROR_ACCESS_LOST;
  auto result = fixture.source->next_frame(0ms);
  REQUIRE_FALSE(result.is_success());
  CHECK(result.error().code == "dxgi_capture.access_lost");
  CHECK(fixture.trace->calls ==
        std::vector<std::string>{"acquire", "release", "acquire"});
  fixture.source.reset();
  CHECK(fixture.trace->destroyed == 1);
  CHECK_FALSE(fixture.trace->destroyed_with_frame);
}

TEST_CASE("Pointer-only updates retain ownership without publishing unchanged pixels") {
  CaptureFixture fixture;
  auto frame = fixture.next();
  frame = {};
  fixture.driver->last_present_time = 0;
  auto pointer_update = fixture.source->next_frame(0ms);
  REQUIRE(pointer_update.is_success());
  CHECK_FALSE(pointer_update.value().has_value());
  CHECK(fixture.trace->frame_owned);
  fixture.trace->desktop_update();
  CHECK(fixture.trace->desktop_copies == 0);
  fixture.driver->last_present_time = 2;
  auto updated = fixture.next();
  CHECK(fixture.trace->calls == std::vector<std::string>{
      "acquire", "release", "acquire", "release", "acquire"});
}

TEST_CASE("First acquired desktop image is delivered even for a pointer-only update") {
  CaptureFixture fixture;
  fixture.driver->last_present_time = 0;
  auto frame = fixture.next();
  CHECK(frame.texture != nullptr);
  CHECK(fixture.trace->frame_owned);
}
