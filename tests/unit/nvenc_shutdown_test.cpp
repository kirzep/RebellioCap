#include "video/nvenc_api.cpp"
#include "video/nvenc_encoder.cpp"
#include <catch2/catch_test_macros.hpp>

namespace rebelliocap {
struct NvencShutdownTestAccess {
  static void check(int failure) {
    auto api_impl = std::make_unique<NvencApi::Impl>();
    api_impl->module = LoadLibraryW(L"kernel32.dll");
    api_impl->functions.nvEncDestroyEncoder = [](void*) -> NVENCSTATUS { return NV_ENC_ERR_GENERIC; };
    api_impl->functions.nvEncUnlockBitstream = [](void*, NV_ENC_OUTPUT_PTR) -> NVENCSTATUS { return NV_ENC_ERR_GENERIC; };
    api_impl->functions.nvEncUnmapInputResource = [](void*, NV_ENC_INPUT_PTR) -> NVENCSTATUS { return NV_ENC_ERR_GENERIC; };
    nvenc_restart_required.store(false);
    QpcClock clock;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 16;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
    auto impl = std::make_unique<NvencEncoder::Impl>(NvencApi(std::move(api_impl)), device.Get(), NvencConfig{}, clock);
    device.Reset();
    auto* retained = impl.get();
    impl->session = reinterpret_cast<void*>(1);
    impl->slots.resize(1);
    auto& slot = impl->slots.front();
    slot.event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const HANDLE event = slot.event.get();
    slot.event_registered = true;
    slot.lifecycle.busy = true;
    slot.lifecycle.mapped = true;
    slot.lifecycle.completion_ready = failure != 0;
    slot.lifecycle.bitstream_locked = failure == 1;
    slot.mapped_input = reinterpret_cast<void*>(2);
    slot.bitstream = reinterpret_cast<void*>(3);
    auto lease = std::make_shared<int>(7);
    std::weak_ptr<int> weak = lease;
    slot.frame = ConvertedVideoFrame(texture, lease);
    lease.reset();
    impl->registrations.push_back(std::make_unique<NvencEncoder::Impl::Registration>());
    impl->registrations.back()->registered = reinterpret_cast<void*>(4);
    impl->registrations.back()->texture = texture;
    texture.Reset();
    slot.registration = impl->registrations.back().get();
    impl->eos_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    impl->eos_event_registered = true;
    const HANDLE eos = impl->eos_event.get();
    auto lifetime = impl->lifetime;
    ++lifetime->state_->in_flight;
    if (failure == 0) {
      impl->pending.push_back(0);
      // A real unsignalled event deterministically hits the production timeout
      // path without a long driver wait or any production fault switches.
      REQUIRE_FALSE(impl->complete_front(1).is_success());
    } else {
      REQUIRE_FALSE(impl->release_completed_slot(slot).is_success());
    }
    auto encoder = std::unique_ptr<NvencEncoder>(new NvencEncoder(std::move(impl)));
    encoder.reset();
    CHECK_FALSE(weak.expired());
    auto rejected = NvencEncoder::create(nullptr, NvencConfig{}, clock);
    REQUIRE_FALSE(rejected.is_success());
    CHECK(rejected.error().code == "nvenc.restart_required");
    DWORD flags = 0;
    CHECK(GetHandleInformation(event, &flags) != FALSE);
    CHECK(GetHandleInformation(eos, &flags) != FALSE);
    CHECK(lifetime->snapshot().in_flight == 1);
    CHECK(lifetime->snapshot().sessions_released == 0);
    // Only inspect retained state after proving that its owning frame survived.
    if (!weak.expired()) {
      CHECK(retained->session != nullptr);
      CHECK(retained->registrations.size() == 1);
      CHECK(retained->device.Get() != nullptr);
      retained->registrations.front()->texture->GetDesc(&desc);
      CHECK(desc.Width == 16);
      CHECK(retained->slots.front().frame->texture() == retained->registrations.front()->texture.Get());
      CHECK(retained->api.implementation_->module != nullptr);
      CHECK(retained->slots[0].mapped_input != nullptr);
      CHECK(lifetime->snapshot().maps_released == 0);
      CHECK(lifetime->snapshot().registrations_released == 0);
      CHECK(lifetime->snapshot().events_released == 0);
      CHECK(lifetime->snapshot().bitstreams_released == 0);
      // Test driver now establishes a successful cancellation boundary.
      retained->api.implementation_->functions.nvEncDestroyEncoder = [](void*) -> NVENCSTATUS { return NV_ENC_SUCCESS; };
      delete retained;
      CHECK(weak.expired());
      CHECK(lifetime->snapshot().in_flight == 0);
      CHECK(lifetime->snapshot().sessions_released == 1);
    }
  }
};
}
TEST_CASE("failed session destruction retains unresolved ownership") {
  SECTION("completion wait unresolved") { rebelliocap::NvencShutdownTestAccess::check(0); }
  SECTION("unlock fails") { rebelliocap::NvencShutdownTestAccess::check(1); }
  SECTION("unmap fails") { rebelliocap::NvencShutdownTestAccess::check(2); }
}
