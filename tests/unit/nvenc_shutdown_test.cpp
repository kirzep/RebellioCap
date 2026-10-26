#include "video/nvenc_api.cpp"
#include "video/nvenc_encoder.cpp"
#include <catch2/catch_test_macros.hpp>

namespace rebelliocap {
struct NvencShutdownTestAccess {
  static void check_profiles() {
    static NvencConfig expected;
    for (const auto preset : {2U, 3U, 4U, 5U}) for (const auto depth : {0U, 1U, 2U}) {
      expected = {}; expected.preset = preset; expected.b_frames = depth;
      expected.multipass = 0;
      auto api = std::make_unique<NvencApi::Impl>();
      api->module = LoadLibraryW(L"kernel32.dll");
      api->functions.nvEncGetEncodePresetConfigEx = [](void*, GUID, GUID selected, NV_ENC_TUNING_INFO, NV_ENC_PRESET_CONFIG* out) -> NVENCSTATUS {
        CHECK(IsEqualGUID(selected, nvenc_preset_guid(expected.preset)));
        out->presetCfg.rcParams.multiPass = NV_ENC_TWO_PASS_FULL_RESOLUTION;
        return NV_ENC_SUCCESS;
      };
      api->functions.nvEncInitializeEncoder = [](void*, NV_ENC_INITIALIZE_PARAMS* in) -> NVENCSTATUS {
        CHECK(in->encodeConfig->frameIntervalP == static_cast<std::int32_t>(expected.b_frames + 1));
        CHECK(in->encodeConfig->rcParams.multiPass == NV_ENC_MULTI_PASS_DISABLED);
        CHECK(IsEqualGUID(in->presetGUID, nvenc_preset_guid(expected.preset)));
        return NV_ENC_SUCCESS;
      };
      api->functions.nvEncGetSequenceParams = [](void*, NV_ENC_SEQUENCE_PARAM_PAYLOAD* out) -> NVENCSTATUS {
        const std::uint8_t bytes[]{0,0,0,1,0x67,100,0,0,0,1,0x68,0};
        std::memcpy(out->spsppsBuffer, bytes, sizeof(bytes));
        *out->outSPSPPSPayloadSize = sizeof(bytes);
        return NV_ENC_SUCCESS;
      };
      QpcClock clock;
      auto impl = std::make_unique<NvencEncoder::Impl>(NvencApi(std::move(api)), nullptr, expected, clock);
      impl->session = reinterpret_cast<void*>(1);
      const auto initialized = impl->initialize_encoder();
      CHECK(initialized.is_success());
      CHECK(impl->accepted.frame_interval_p == depth + 1);
      CHECK(impl->accepted.preset_number == preset);
      CHECK(impl->accepted.multipass == 0);
      impl->initialized = false; impl->session = nullptr;
    }
  }
  static void check_backpressure(std::size_t capacity = kAsyncSlotCount, bool admission = false, bool watchdog = false) {
    auto api_impl = std::make_unique<NvencApi::Impl>();
    api_impl->module = LoadLibraryW(L"kernel32.dll");
    auto& functions = api_impl->functions;
    functions.nvEncDestroyEncoder = [](void*) -> NVENCSTATUS { return NV_ENC_SUCCESS; };
    functions.nvEncUnlockBitstream = [](void*, NV_ENC_OUTPUT_PTR) -> NVENCSTATUS { return NV_ENC_SUCCESS; };
    functions.nvEncUnmapInputResource = [](void*, NV_ENC_INPUT_PTR) -> NVENCSTATUS { return NV_ENC_SUCCESS; };
    functions.nvEncMapInputResource = [](void*, NV_ENC_MAP_INPUT_RESOURCE* input) -> NVENCSTATUS {
      input->mappedResource = reinterpret_cast<void*>(2);
      input->mappedBufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
      return NV_ENC_SUCCESS;
    };
    functions.nvEncEncodePicture = [](void*, NV_ENC_PIC_PARAMS* picture) -> NVENCSTATUS {
      CHECK((picture->encodePicFlags & NV_ENC_PIC_FLAG_FORCEIDR) != 0);
      CHECK((picture->encodePicFlags & NV_ENC_PIC_FLAG_OUTPUT_SPSPPS) != 0);
      return NV_ENC_SUCCESS;
    };
    QpcClock clock;
    const auto duration = clock.frequency() / 60;
    functions.nvEncLockBitstream = [](void*, NV_ENC_LOCK_BITSTREAM* output) -> NVENCSTATUS {
      static const std::byte payload[]{std::byte{1}};
      QpcClock clock;
      output->bitstreamBufferPtr = const_cast<std::byte*>(payload);
      output->bitstreamSizeInBytes = 1;
      output->outputTimeStamp = 0;
      output->outputDuration = clock.frequency() / 60;
      output->pictureType = NV_ENC_PIC_TYPE_IDR;
      return NV_ENC_SUCCESS;
    };
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 16;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_NV12;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
    NvencConfig config{16, 16, 60}; config.slot_count = static_cast<std::uint32_t>(capacity);
    auto impl = std::make_unique<NvencEncoder::Impl>(NvencApi(std::move(api_impl)),
        device.Get(), config, clock);
    impl->session = reinterpret_cast<void*>(1);
    impl->registrations.push_back(std::make_unique<NvencEncoder::Impl::Registration>());
    impl->registrations.back()->texture = texture;
    impl->registrations.back()->registered = reinterpret_cast<void*>(4);
    auto lease = std::make_shared<int>(7);
    impl->slots.resize(capacity);
    for (std::size_t i = 0; i < impl->slots.size(); ++i) {
      auto& slot = impl->slots[i];
      slot.event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      slot.lifecycle.busy = slot.lifecycle.mapped = true;
      slot.mapped_input = reinterpret_cast<void*>(2);
      slot.frame = ConvertedVideoFrame(texture, lease);
      slot.duration = duration;
      // An outstanding frame older than the former timeout is still queue
      // pressure during normal capture, and remains visible in diagnostics.
      slot.submitted_at = clock.now() - 12 * clock.frequency();
      impl->pending.push_back(i);
      impl->decode_timeline.submit(static_cast<QpcTicks>(i) * duration, duration);
      ++impl->lifetime->state_->in_flight;
    }
    impl->last_submitted_pts = static_cast<QpcTicks>(capacity - 1) * duration;
    // A busy queue must not wait for the driver or become a terminal error.
    // Repeated drops must preserve both outstanding texture leases and IDR.
    for (std::size_t i = capacity; i < capacity + 3; ++i) {
      const auto begin = clock.now();
      if (admission) {
        auto prepared = impl->prepare_frame(i == capacity);
        REQUIRE(prepared.is_success());
        CHECK_FALSE(prepared.value().can_submit);
        CHECK(prepared.value().completed.empty());
        CHECK(impl->pending_keyframe);
        CHECK(impl->pending.size() == capacity);
        CHECK(clock.now() - begin < clock.frequency());
        continue;
      }
      auto result = impl->encode_frame(ConvertedVideoFrame(texture, lease), static_cast<QpcTicks>(i) * duration, i == capacity);
      CHECK(clock.now() - begin < clock.frequency());
      REQUIRE(result.is_success());
      CHECK(result.value().empty());
      CHECK(impl->pending.size() == capacity);
      CHECK(impl->state.state() == detail::NvencEncoderState::Accepting);
      CHECK(impl->lifetime->snapshot().maps_released == 0);
    }
    if (watchdog) {
      impl->config.completion_stall_timeout_ms = 1000;
      auto stalled = impl->prepare_frame(false);
      REQUIRE_FALSE(stalled.is_success());
      CHECK(stalled.error().code == "nvenc.encoder_stalled");
      CHECK(impl->state.state() == detail::NvencEncoderState::Failed);
      CHECK(impl->lifetime->snapshot().maps_released == 0);
      CHECK(impl->pending.size() == capacity);
      return;
    }
    REQUIRE(SetEvent(impl->slots.front().event.get()) != FALSE);
    if (admission) {
      auto prepared = impl->prepare_frame(false);
      REQUIRE(prepared.is_success());
      REQUIRE(prepared.value().can_submit);
      REQUIRE(prepared.value().completed.size() == 1);
      CHECK(prepared.value().completed.front().pts == 0);
      CHECK(impl->lifetime->snapshot().maps_released == 1);
      CHECK(impl->pending_keyframe);
    }
    const auto resume_pts = static_cast<QpcTicks>(capacity + 3) * duration;
    auto resumed = impl->encode_frame(ConvertedVideoFrame(texture, lease), resume_pts, false);
    REQUIRE(resumed.is_success());
    REQUIRE(resumed.value().size() == (admission ? 0 : 1));
    if (!admission) CHECK(resumed.value().front().pts == 0);
    CHECK(impl->submitted_frames == 1);
    CHECK(impl->completed_frames == 1);
    CHECK(impl->last_submitted_pts == resume_pts);
    CHECK(impl->backpressure_dropped_frames == 3);
    CHECK_FALSE(impl->pending_keyframe);
    auto encoder = std::unique_ptr<NvencEncoder>(new NvencEncoder(std::move(impl)));
    const auto metrics = encoder->metrics();
    CHECK(metrics.backpressure_dropped_frames == 3);
    CHECK(metrics.in_flight_frames == capacity);
    CHECK(metrics.oldest_in_flight_age_us >= 12'000'000);
  }
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
TEST_CASE("NVENC queue pressure drops input and resumes with the requested keyframe") {
  rebelliocap::NvencShutdownTestAccess::check_backpressure();
}
TEST_CASE("NVENC admission frees surfaces before conversion for bounded pool capacities") {
  for (const auto capacity : {6U, 8U, 10U, 12U}) {
    CAPTURE(capacity);
    rebelliocap::NvencShutdownTestAccess::check_backpressure(capacity, true);
  }
}
TEST_CASE("NVENC internal profiles honor preset B depth and explicit multipass") {
  rebelliocap::NvencShutdownTestAccess::check_profiles();
}
TEST_CASE("NVENC stalled completion fails explicitly without releasing driver surfaces") {
  rebelliocap::NvencShutdownTestAccess::check_backpressure(6, true, true);
}
TEST_CASE("failed session destruction retains unresolved ownership") {
  SECTION("completion wait unresolved") { rebelliocap::NvencShutdownTestAccess::check(0); }
  SECTION("unlock fails") { rebelliocap::NvencShutdownTestAccess::check(1); }
  SECTION("unmap fails") { rebelliocap::NvencShutdownTestAccess::check(2); }
}
