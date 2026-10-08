#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <utility>

#include <nlohmann/json.hpp>
#include <wrl/client.h>

#include "capture/capture_source.h"
#include "video/d3d11_nv12_converter.h"

namespace {

constexpr int kSkipped = 77;
constexpr std::uint32_t kInputWidth = 1280;
constexpr std::uint32_t kInputHeight = 720;
constexpr std::uint32_t kOutputWidth = 640;
constexpr std::uint32_t kOutputHeight = 360;
constexpr std::size_t kPoolSize = 3;
constexpr int kRepeatConversions = 24;

int fail(const std::string& reason) {
  std::cerr << "FAIL: " << reason << '\n';
  return 1;
}

struct TestDevice {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  bool debug_flag_succeeded{false};
};

bool is_precisely_absent_hardware(HRESULT result) {
  return result == DXGI_ERROR_UNSUPPORTED;
}

rebelliocap::Result<TestDevice> create_test_device() {
  constexpr std::array feature_levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

  TestDevice created;
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;
  HRESULT result = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, feature_levels.data(),
      static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION, &created.device, nullptr,
      &created.context);
  if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    created.device.Reset();
    created.context.Reset();
    result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, feature_levels.data(),
        static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION, &created.device, nullptr,
        &created.context);
  }

  if (result == E_INVALIDARG) {
    created.device.Reset();
    created.context.Reset();
    result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               feature_levels.data() + 1, 1, D3D11_SDK_VERSION,
                               &created.device, nullptr, &created.context);
  }
  if (FAILED(result)) {
    return rebelliocap::Result<TestDevice>::failure(rebelliocap::Error{
        "nv12_test.device_unavailable", "A hardware D3D11 device is unavailable.",
        static_cast<long>(result)});
  }
  created.debug_flag_succeeded = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
  return rebelliocap::Result<TestDevice>::success(std::move(created));
}

rebelliocap::Result<rebelliocap::CapturedVideoFrame> make_input_frame(
    ID3D11Device* device, ID3D11DeviceContext* context,
    std::uint32_t width = kInputWidth, std::uint32_t height = kInputHeight) {
  D3D11_TEXTURE2D_DESC description{};
  description.Width = width;
  description.Height = height;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  HRESULT result = device->CreateTexture2D(&description, nullptr, &texture);
  if (FAILED(result)) {
    return rebelliocap::Result<rebelliocap::CapturedVideoFrame>::failure(
        rebelliocap::Error{"nv12_test.input_texture_failed",
                            "Could not create the deterministic BGRA input texture.",
                            static_cast<long>(result)});
  }

  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
  result = device->CreateRenderTargetView(texture.Get(), nullptr, &target);
  if (FAILED(result)) {
    return rebelliocap::Result<rebelliocap::CapturedVideoFrame>::failure(
        rebelliocap::Error{"nv12_test.input_view_failed",
                            "Could not create the deterministic BGRA render-target view.",
                            static_cast<long>(result)});
  }
  constexpr std::array<float, 4> clear_color{0.125F, 0.5F, 0.875F, 1.0F};
  context->ClearRenderTargetView(target.Get(), clear_color.data());

  rebelliocap::CapturedVideoFrame frame;
  frame.texture = std::move(texture);
  frame.captured_at = 1;
  frame.width = width;
  frame.height = height;
  frame.lifetime = std::make_shared<const int>(1);
  return rebelliocap::Result<rebelliocap::CapturedVideoFrame>::success(std::move(frame));
}

bool same_com_identity(IUnknown* left, IUnknown* right) {
  Microsoft::WRL::ComPtr<IUnknown> left_identity;
  Microsoft::WRL::ComPtr<IUnknown> right_identity;
  return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&left_identity))) &&
         SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&right_identity))) &&
         left_identity.Get() == right_identity.Get();
}

bool check_rotation(TestDevice& device, DXGI_MODE_ROTATION rotation,
                    const std::array<int, 4>& expected) {
  constexpr UINT width = 64, height = 32;
  auto frame = make_input_frame(device.device.Get(), device.context.Get(), width, height);
  if (!frame.is_success()) return false;
  std::vector<std::uint32_t> pixels(width * height);
  constexpr std::array<UINT, 4> shades{32, 96, 160, 224};
  for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
    const auto shade = shades[(y >= height / 2 ? 2 : 0) + (x >= width / 2 ? 1 : 0)];
    pixels[y * width + x] = 0xff000000U | shade * 0x010101U;
  }
  device.context->UpdateSubresource(frame.value().texture.Get(), 0, nullptr,
                                     pixels.data(), width * 4, 0);
  const bool portrait = rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
  const UINT out_width = portrait ? height : width, out_height = portrait ? width : height;
  auto converter = rebelliocap::D3d11Nv12Converter::create(device.device.Get(), device.context.Get(),
      {width, height, out_width, out_height, 1, rotation});
  if (!converter.is_success()) { std::cerr << converter.error().code << '\n'; return false; }
  auto converted = converter.value()->convert(frame.value());
  if (!converted.is_success()) return false;
  D3D11_TEXTURE2D_DESC desc{};
  converted.value().texture()->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device.device->CreateTexture2D(&desc, nullptr, &staging))) return false;
  device.context->CopyResource(staging.Get(), converted.value().texture());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(device.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
  bool valid = true;
  for (UINT quadrant = 0; quadrant < 4; ++quadrant) {
    const UINT x = out_width * (quadrant % 2 ? 3 : 1) / 4;
    const UINT y = out_height * (quadrant / 2 ? 3 : 1) / 4;
    const int luma = static_cast<const std::uint8_t*>(mapped.pData)[y * mapped.RowPitch + x];
    if (std::abs(luma - expected[quadrant]) > 5) {
      std::cerr << "rotation=" << rotation << " quadrant=" << quadrant << " luma=" << luma << " expected=" << expected[quadrant] << '\n';
      valid = false;
    }
  }
  device.context->Unmap(staging.Get(), 0);
  return valid;
}

std::vector<std::string> relevant_debug_messages(ID3D11InfoQueue* queue) {
  if (queue == nullptr) {
    return {};
  }

  std::vector<std::string> messages;
  const auto count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
  for (UINT64 index = 0; index < count; ++index) {
    SIZE_T bytes = 0;
    if (FAILED(queue->GetMessage(index, nullptr, &bytes)) || bytes == 0) {
      continue;
    }
    std::vector<std::byte> storage(bytes);
    auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
    if (FAILED(queue->GetMessage(index, message, &bytes))) {
      continue;
    }
    if (message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ||
        message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ||
        message->Severity == D3D11_MESSAGE_SEVERITY_WARNING) {
      messages.emplace_back(message->pDescription, message->DescriptionByteLength);
    }
  }
  return messages;
}

}  // namespace

int main() {
  using namespace rebelliocap;

  if (!is_precisely_absent_hardware(DXGI_ERROR_UNSUPPORTED) ||
      is_precisely_absent_hardware(E_FAIL) ||
      is_precisely_absent_hardware(E_ACCESSDENIED) ||
      is_precisely_absent_hardware(E_OUTOFMEMORY)) {
    return fail("D3D11 hardware-unavailable skip classification is not exact");
  }

  auto device_result = create_test_device();
  if (!device_result.is_success()) {
    const auto result = static_cast<HRESULT>(device_result.error().hresult.value_or(E_FAIL));
    if (is_precisely_absent_hardware(result)) {
      std::cerr << "SKIP: no suitable hardware D3D11 device is available. HRESULT="
                << result << '\n';
      return kSkipped;
    }
    return fail("D3D11 hardware device creation failed: " +
                device_result.error().code + " HRESULT=" + std::to_string(result));
  }
  auto test_device = std::move(device_result).value();

  if (!check_rotation(test_device, DXGI_MODE_ROTATION_IDENTITY, {43, 98, 153, 208}) ||
      !check_rotation(test_device, DXGI_MODE_ROTATION_ROTATE90, {153, 43, 208, 98}) ||
      !check_rotation(test_device, DXGI_MODE_ROTATION_ROTATE180, {208, 153, 98, 43}) ||
      !check_rotation(test_device, DXGI_MODE_ROTATION_ROTATE270, {98, 208, 43, 153}))
    return fail("GPU rotation did not preserve the four asymmetric quadrants");

  Microsoft::WRL::ComPtr<ID3D11InfoQueue> info_queue;
  if (test_device.debug_flag_succeeded) {
    const HRESULT result = test_device.device.As(&info_queue);
    if (FAILED(result)) {
      return fail("D3D11 debug flag succeeded but ID3D11InfoQueue is unavailable: HRESULT=" +
                  std::to_string(result));
    }
    info_queue->ClearStoredMessages();
  }
  const bool debug_layer_available = info_queue != nullptr;

  const D3d11Nv12ConverterConfig invalid_config{
      kInputWidth, kInputHeight, kOutputWidth + 1, kOutputHeight, kPoolSize};
  auto invalid_result = D3d11Nv12Converter::create(
      test_device.device.Get(), test_device.context.Get(), invalid_config);
  if (invalid_result.is_success() ||
      invalid_result.error().code != "nv12_converter.invalid_output_dimensions") {
    return fail("odd NV12 dimensions did not produce the exact actionable error");
  }

  const D3d11Nv12ConverterConfig config{
      kInputWidth, kInputHeight, kOutputWidth, kOutputHeight, kPoolSize};
  auto converter_result = D3d11Nv12Converter::create(
      test_device.device.Get(), test_device.context.Get(), config);
  if (!converter_result.is_success()) {
    return fail("converter creation failed: " + converter_result.error().code);
  }
  auto converter = std::move(converter_result).value();

  auto input_result = make_input_frame(test_device.device.Get(), test_device.context.Get());
  if (!input_result.is_success()) {
    return fail(input_result.error().message);
  }
  auto input = std::move(input_result).value();

  std::optional<ConvertedVideoFrame> first;
  std::optional<ConvertedVideoFrame> second;
  std::optional<ConvertedVideoFrame> third;
  for (auto* retained : {&first, &second, &third}) {
    auto conversion = converter->convert(input);
    if (!conversion.is_success()) {
      return fail("retained pool conversion failed: " + conversion.error().code);
    }
    retained->emplace(std::move(conversion).value());
  }
  auto exhausted = converter->convert(input);
  if (exhausted.is_success() ||
      exhausted.error().code != "nv12_converter.output_pool_exhausted") {
    return fail("converter overwrote or ambiguously reused a caller-retained pool surface");
  }
  first.reset();
  {
    auto after_release = converter->convert(input);
    if (!after_release.is_success()) {
      return fail("released output surface did not return to the pool");
    }
  }
  second.reset();
  third.reset();

  D3D11_TEXTURE2D_DESC output_description{};
  for (int iteration = 0; iteration < kRepeatConversions; ++iteration) {
    auto conversion = converter->convert(input);
    if (!conversion.is_success()) {
      return fail("repeat conversion failed: " + conversion.error().code);
    }
    auto output = std::move(conversion).value();
    if (output.texture() == nullptr) {
      return fail("conversion returned a null output texture");
    }
    output.texture()->GetDesc(&output_description);
    if (output_description.Width != kOutputWidth ||
        output_description.Height != kOutputHeight ||
        output_description.Format != DXGI_FORMAT_NV12 ||
        output_description.Usage != D3D11_USAGE_DEFAULT ||
        output_description.CPUAccessFlags != 0 ||
        (output_description.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 ||
        (output_description.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
      return fail("output descriptor violates the GPU-only NV12 contract");
    }

    Microsoft::WRL::ComPtr<ID3D11Device> output_device;
    output.texture()->GetDevice(&output_device);
    if (!same_com_identity(output_device.Get(), test_device.device.Get())) {
      return fail("NV12 output texture was created on a different D3D11 device");
    }
  }

  const auto processor_state = converter->processor_state();
  if (converter->metrics().input_views_created != 1) {
    return fail("repeated conversions rebuilt the unchanged BGRA input view");
  }

  const auto before_cached = converter->metrics();
  for (int iteration = 0; iteration < kRepeatConversions; ++iteration) {
    auto conversion = converter->convert_cached(input, 1);
    if (!conversion.is_success()) {
      return fail("cached conversion failed: " + conversion.error().code);
    }
  }
  if (converter->metrics().processing_submissions !=
          before_cached.processing_submissions + 1 ||
      converter->metrics().cached_conversions != kRepeatConversions - 1) {
    return fail("unchanged image submitted repeated video-processing GPU work");
  }
  const auto unchanged_submissions = converter->metrics().processing_submissions;
  {
    auto updated = converter->convert_cached(input, 2);
    if (!updated.is_success() || converter->metrics().processing_submissions != unchanged_submissions + 1) {
      return fail("changed image revision incorrectly reused old NV12 pixels");
    }
  }
  const auto pool_submissions = converter->metrics().processing_submissions;
  for (auto* retained : {&first, &second, &third}) {
    auto conversion = converter->convert_cached(input, 2);
    if (!conversion.is_success()) return fail("cached retained pool conversion failed");
    retained->emplace(std::move(conversion).value());
  }
  auto cached_exhausted = converter->convert_cached(input, 2);
  if (cached_exhausted.is_success() ||
      cached_exhausted.error().code != "nv12_converter.output_pool_exhausted" ||
      converter->metrics().processing_submissions != pool_submissions + 2) {
    return fail("cached conversion reused a leased output or skipped filling a fresh pool slot");
  }
  first.reset(); second.reset(); third.reset();
  {
    auto uncached = converter->convert(input);
    if (!uncached.is_success()) return fail("uncached conversion after caching failed");
  }
  const auto uncached_submissions = converter->metrics().processing_submissions;
  {
    auto cached_again = converter->convert_cached(input, 2);
    if (!cached_again.is_success() || converter->metrics().processing_submissions != uncached_submissions + 1) {
      return fail("uncached conversion did not invalidate cached content");
    }
  }
  {
    auto other_input = make_input_frame(test_device.device.Get(), test_device.context.Get());
    if (!other_input.is_success()) return fail("second input allocation failed");
    const auto before = converter->metrics();
    auto different_texture = converter->convert_cached(other_input.value(), 2);
    if (!different_texture.is_success() || converter->metrics().input_views_created != before.input_views_created + 1 ||
        converter->metrics().processing_submissions != before.processing_submissions + 1) {
      return fail("same revision on a different input texture incorrectly reused cached pixels");
    }
  }
  if (processor_state.input_color_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
      processor_state.output_color_space !=
          DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709 ||
      processor_state.auto_processing_enabled) {
    return fail("queried video-processor state violates the explicit color contract");
  }

  // Resolution changes must preserve output textures already registered with
  // NVENC and every active lease, including exhaustion across reconfiguration.
  std::vector<rebelliocap::ConvertedVideoFrame> old_leases;
  std::vector<ID3D11Texture2D*> surfaces;
  for (std::size_t i = 0; i < kPoolSize; ++i) {
    auto frame = converter->convert(input);
    if (!frame.is_success()) return fail("initial mode lease failed");
    surfaces.push_back(frame.value().texture());
    old_leases.push_back(std::move(frame).value());
  }
  if (converter->reconfigure_input(0, 600).is_success() || converter->config().input_width != kInputWidth)
    return fail("invalid reconfiguration changed committed dimensions");
  for (const auto dimensions : {std::pair{800U,600U}, std::pair{1920U,1080U}, std::pair{kInputWidth,kInputHeight}}) {
    auto rebuilt = converter->reconfigure_input(dimensions.first, dimensions.second);
    if (!rebuilt.is_success()) return fail("input reconfiguration failed: " + rebuilt.error().message);
    auto changed = make_input_frame(test_device.device.Get(), test_device.context.Get(), dimensions.first, dimensions.second);
    if (!changed.is_success()) return fail("changed input allocation failed");
    auto mode_exhausted = converter->convert(changed.value());
    if (mode_exhausted.is_success() || mode_exhausted.error().code != "nv12_converter.output_pool_exhausted")
      return fail("reconfiguration lost output leases");
    old_leases.clear();
    for (std::size_t i = 0; i < kPoolSize; ++i) {
      auto frame = converter->convert(changed.value());
      if (!frame.is_success() || frame.value().texture() != surfaces[i]) return fail("reconfiguration replaced registered output textures");
      old_leases.push_back(std::move(frame).value());
    }
  }
  old_leases.clear();

  // Rotation-only recovery must also keep registered surfaces and invalidate
  // cached pixels, even when capture dimensions are unchanged.
  const auto before_rotation = converter->metrics().processing_submissions;
  if (converter->reconfigure_input(kInputWidth, kInputHeight, DXGI_MODE_ROTATION_ROTATE180).is_success() == false)
    return fail("rotation-only reconfiguration failed");
  {
    auto rotated = converter->convert_cached(input, 2);
    if (!rotated.is_success() || rotated.value().texture() != surfaces[0] ||
        converter->config().rotation != DXGI_MODE_ROTATION_ROTATE180 ||
        converter->metrics().processing_submissions != before_rotation + 1)
      return fail("rotation recovery replaced a surface or reused stale pixels");
  }
  if (converter->reconfigure_input(kInputWidth, kInputHeight, static_cast<DXGI_MODE_ROTATION>(999)).is_success() ||
      converter->config().rotation != DXGI_MODE_ROTATION_ROTATE180)
    return fail("invalid rotation changed the committed converter");

  test_device.context->Flush();
  const auto debug_messages = relevant_debug_messages(info_queue.Get());
  if (!debug_messages.empty()) {
    for (const auto& message : debug_messages) {
      std::cerr << "D3D11: " << message << '\n';
    }
    return fail("D3D11 debug layer reported resource misuse warnings or errors");
  }

  nlohmann::json evidence = {
      {"schema", 1},
      {"type", "nv12_converter_evidence"},
      {"input_width", kInputWidth},
      {"input_height", kInputHeight},
      {"input_format", static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM)},
      {"output_width", output_description.Width},
      {"output_height", output_description.Height},
      {"output_format", static_cast<std::uint32_t>(output_description.Format)},
      {"output_usage", static_cast<std::uint32_t>(output_description.Usage)},
      {"output_bind_flags", output_description.BindFlags},
      {"output_cpu_access", output_description.CPUAccessFlags},
      {"input_color_space",
       static_cast<std::uint32_t>(processor_state.input_color_space)},
      {"output_color_space",
       static_cast<std::uint32_t>(processor_state.output_color_space)},
      {"auto_processing_enabled", processor_state.auto_processing_enabled},
      {"pool_size", kPoolSize},
      {"repeat_conversions", kRepeatConversions},
      {"input_views_created", converter->metrics().input_views_created},
      {"processing_submissions", converter->metrics().processing_submissions},
      {"cached_conversions", converter->metrics().cached_conversions},
      {"pool_exhaustion_proved", true},
      {"asymmetric_rotation_cases", 4},
      {"rotation_only_reconfiguration", true},
      {"same_device", true},
      {"debug_layer_available", debug_layer_available},
      {"debug_misuse_messages", debug_messages.size()},
      {"gpu_only", output_description.Usage == D3D11_USAGE_DEFAULT &&
                       output_description.CPUAccessFlags == 0}};
  std::cout << evidence.dump() << '\n';
  return 0;
}
