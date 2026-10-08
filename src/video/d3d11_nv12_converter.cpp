#include "video/d3d11_nv12_converter.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace rebelliocap {
namespace {

constexpr DXGI_COLOR_SPACE_TYPE kInputColorSpace =
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
constexpr DXGI_COLOR_SPACE_TYPE kOutputColorSpace =
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;

Error converter_error(std::string code, std::string message,
                      std::optional<long> hresult = std::nullopt) {
  return Error{std::move(code), std::move(message), hresult};
}

Error hresult_error(std::string code, std::string message, HRESULT result) {
  return converter_error(std::move(code), std::move(message), static_cast<long>(result));
}

bool same_com_identity(IUnknown* left, IUnknown* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  Microsoft::WRL::ComPtr<IUnknown> left_identity;
  Microsoft::WRL::ComPtr<IUnknown> right_identity;
  return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&left_identity))) &&
         SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&right_identity))) &&
         left_identity.Get() == right_identity.Get();
}

struct OutputSlot {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view;
  bool leased{false};
  std::optional<std::uint64_t> revision;
};

struct OutputPoolState {
  std::mutex mutex;
  std::vector<OutputSlot> slots;
};

class OutputLease final {
 public:
  OutputLease(std::shared_ptr<OutputPoolState> pool, std::size_t index) noexcept
      : pool_(std::move(pool)), index_(index) {}

  OutputLease(const OutputLease&) = delete;
  OutputLease& operator=(const OutputLease&) = delete;

  ~OutputLease() {
    const std::lock_guard lock(pool_->mutex);
    pool_->slots[index_].leased = false;
  }

 private:
  std::shared_ptr<OutputPoolState> pool_;
  std::size_t index_;
};

struct AcquiredOutput {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view;
  std::shared_ptr<const void> lease;
  std::size_t index;
};

Result<AcquiredOutput> acquire_output(const std::shared_ptr<OutputPoolState>& pool) {
  const std::lock_guard lock(pool->mutex);
  const auto available = std::find_if(pool->slots.begin(), pool->slots.end(),
                                      [](const OutputSlot& slot) { return !slot.leased; });
  if (available == pool->slots.end()) {
    return Result<AcquiredOutput>::failure(converter_error(
        "nv12_converter.output_pool_exhausted",
        "Every NV12 output surface is still retained by a caller."));
  }

  const auto index = static_cast<std::size_t>(available - pool->slots.begin());
  available->leased = true;
  auto lease = std::make_shared<const OutputLease>(pool, index);
  return Result<AcquiredOutput>::success(
      AcquiredOutput{available->texture, available->view, std::move(lease), index});
}

Result<void> validate_config(const D3d11Nv12ConverterConfig& config) {
  if (config.rotation < DXGI_MODE_ROTATION_UNSPECIFIED || config.rotation > DXGI_MODE_ROTATION_ROTATE270)
    return Result<void>::failure(converter_error("nv12_converter.invalid_rotation", "Unknown desktop rotation."));
  if (config.input_width == 0 || config.input_height == 0) {
    return Result<void>::failure(converter_error(
        "nv12_converter.invalid_input_dimensions",
        "BGRA input dimensions must both be nonzero."));
  }
  if (config.output_width == 0 || config.output_height == 0 ||
      (config.output_width % 2U) != 0 || (config.output_height % 2U) != 0) {
    return Result<void>::failure(converter_error(
        "nv12_converter.invalid_output_dimensions",
        "NV12 output dimensions must be nonzero even values."));
  }
  if (config.output_pool_size == 0 ||
      config.output_pool_size >
          static_cast<std::size_t>((std::numeric_limits<UINT>::max)())) {
    return Result<void>::failure(converter_error(
        "nv12_converter.invalid_pool_size",
        "The NV12 output pool must contain at least one addressable surface."));
  }
  return Result<void>::success();
}

}  // namespace

struct D3d11Nv12Converter::Impl {
  D3d11Nv12ConverterConfig config{};
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device;
  Microsoft::WRL::ComPtr<ID3D11VideoContext1> video_context;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator1> enumerator1;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
  std::shared_ptr<OutputPoolState> output_pool;
  std::mutex conversion_mutex;
  D3d11Nv12RuntimeMetrics metrics;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> input_texture;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
};

D3d11Nv12Converter::D3d11Nv12Converter(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

Result<void> D3d11Nv12Converter::reconfigure_input(std::uint32_t width, std::uint32_t height, DXGI_MODE_ROTATION rotation) {
  auto config = implementation_->config;
  config.input_width = width;
  config.input_height = height;
  config.rotation = rotation;
  auto created = create(implementation_->device.Get(), implementation_->context.Get(), config);
  if (!created.is_success()) return Result<void>::failure(created.error());
  auto replacement = std::move(created).value();
  auto& next = *replacement->implementation_;
  auto pool = implementation_->output_pool;
  std::vector<Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>> views;
  {
    const std::lock_guard lock(pool->mutex);
    views.reserve(pool->slots.size());
    for (const auto& slot : pool->slots) {
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC description{};
      description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view;
      const HRESULT result = next.video_device->CreateVideoProcessorOutputView(
          slot.texture.Get(), next.enumerator.Get(), &description, &view);
      if (FAILED(result)) return Result<void>::failure(hresult_error(
          "nv12_converter.output_view_creation_failed", "Could not rebind the retained NV12 pool.", result));
      views.push_back(std::move(view));
    }
    for (std::size_t index = 0; index < views.size(); ++index) {
      pool->slots[index].view = std::move(views[index]);
      pool->slots[index].revision.reset();
    }
  }
  next.output_pool = std::move(pool);
  next.metrics = implementation_->metrics;
  implementation_ = std::move(replacement->implementation_);
  return Result<void>::success();
}

D3d11Nv12Converter::~D3d11Nv12Converter() = default;

Result<std::unique_ptr<D3d11Nv12Converter>> D3d11Nv12Converter::create(
    ID3D11Device* device, ID3D11DeviceContext* context,
    const D3d11Nv12ConverterConfig& config) {
  auto validation = validate_config(config);
  if (!validation.is_success()) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(
        std::move(validation).error());
  }
  if (device == nullptr || context == nullptr) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(converter_error(
        "nv12_converter.invalid_device",
        "A D3D11 device and its immediate context are required."));
  }

  Microsoft::WRL::ComPtr<ID3D11Device> context_device;
  context->GetDevice(&context_device);
  if (!same_com_identity(device, context_device.Get())) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(converter_error(
        "nv12_converter.context_device_mismatch",
        "The immediate context belongs to a different D3D11 device."));
  }

  auto implementation = std::make_unique<Impl>();
  implementation->config = config;
  implementation->device = device;
  implementation->context = context;

  HRESULT result = device->QueryInterface(IID_PPV_ARGS(&implementation->video_device));
  if (FAILED(result)) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(hresult_error(
        "nv12_converter.video_device_unavailable",
        "The D3D11 device does not expose video-processing support.", result));
  }
  result = context->QueryInterface(IID_PPV_ARGS(&implementation->video_context));
  if (FAILED(result)) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(hresult_error(
        "nv12_converter.explicit_color_space_unavailable",
        "The D3D11 context cannot explicitly configure Windows 10 video color spaces.",
        result));
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputFrameRate = DXGI_RATIONAL{60, 1};
  content.InputWidth = config.input_width;
  content.InputHeight = config.input_height;
  content.OutputFrameRate = DXGI_RATIONAL{60, 1};
  content.OutputWidth = config.output_width;
  content.OutputHeight = config.output_height;
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  result = implementation->video_device->CreateVideoProcessorEnumerator(
      &content, &implementation->enumerator);
  if (FAILED(result)) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(hresult_error(
        "nv12_converter.enumerator_creation_failed",
        "The GPU rejected the requested BGRA-to-NV12 dimensions.", result));
  }
  result = implementation->enumerator.As(&implementation->enumerator1);
  if (FAILED(result)) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(hresult_error(
        "nv12_converter.color_conversion_query_unavailable",
        "The GPU runtime cannot validate explicit format and color-space conversion.",
        result));
  }

  BOOL conversion_supported = FALSE;
  result = implementation->enumerator1->CheckVideoProcessorFormatConversion(
      DXGI_FORMAT_B8G8R8A8_UNORM, kInputColorSpace, DXGI_FORMAT_NV12,
      kOutputColorSpace, &conversion_supported);
  if (FAILED(result)) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(hresult_error(
        "nv12_converter.color_conversion_query_failed",
        "The GPU failed to validate the required BT.709 color conversion.", result));
  }
  if (conversion_supported != TRUE) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(converter_error(
        "nv12_converter.color_conversion_unsupported",
        "The GPU does not support full-range RGB BT.709 to limited-range NV12 BT.709."));
  }

  UINT input_support = 0;
  result = implementation->enumerator->CheckVideoProcessorFormat(
      DXGI_FORMAT_B8G8R8A8_UNORM, &input_support);
  if (FAILED(result) || (input_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(hresult_error(
        "nv12_converter.bgra_input_unsupported",
        "The GPU video processor does not accept BGRA8 input.", result));
  }
  UINT output_support = 0;
  result = implementation->enumerator->CheckVideoProcessorFormat(
      DXGI_FORMAT_NV12, &output_support);
  if (FAILED(result) || (output_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(hresult_error(
        "nv12_converter.nv12_output_unsupported",
        "The GPU video processor does not produce NV12 output.", result));
  }

  result = implementation->video_device->CreateVideoProcessor(
      implementation->enumerator.Get(), 0, &implementation->processor);
  if (FAILED(result)) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(hresult_error(
        "nv12_converter.processor_creation_failed",
        "Could not create the reusable D3D11 video processor.", result));
  }

  implementation->output_pool = std::make_shared<OutputPoolState>();
  implementation->output_pool->slots.reserve(config.output_pool_size);
  for (std::size_t index = 0; index < config.output_pool_size; ++index) {
    D3D11_TEXTURE2D_DESC texture_description{};
    texture_description.Width = config.output_width;
    texture_description.Height = config.output_height;
    texture_description.MipLevels = 1;
    texture_description.ArraySize = 1;
    texture_description.Format = DXGI_FORMAT_NV12;
    texture_description.SampleDesc.Count = 1;
    texture_description.Usage = D3D11_USAGE_DEFAULT;
    texture_description.BindFlags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    OutputSlot slot;
    result = device->CreateTexture2D(&texture_description, nullptr, &slot.texture);
    if (FAILED(result)) {
      return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(hresult_error(
          "nv12_converter.output_texture_creation_failed",
          "Could not allocate a GPU-only pooled NV12 output surface.", result));
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_description{};
    view_description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    view_description.Texture2D.MipSlice = 0;
    result = implementation->video_device->CreateVideoProcessorOutputView(
        slot.texture.Get(), implementation->enumerator.Get(), &view_description, &slot.view);
    if (FAILED(result)) {
      return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(hresult_error(
          "nv12_converter.output_view_creation_failed",
          "Could not bind a pooled NV12 surface as video-processor output.", result));
    }
    implementation->output_pool->slots.push_back(std::move(slot));
  }

  implementation->video_context->VideoProcessorSetStreamFrameFormat(
      implementation->processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  implementation->video_context->VideoProcessorSetStreamColorSpace1(
      implementation->processor.Get(), 0, kInputColorSpace);
  implementation->video_context->VideoProcessorSetOutputColorSpace1(
      implementation->processor.Get(), kOutputColorSpace);
  implementation->video_context->VideoProcessorSetStreamAutoProcessingMode(
      implementation->processor.Get(), 0, FALSE);

  const bool rotated = config.rotation >= DXGI_MODE_ROTATION_ROTATE90;
  if (rotated) {
    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    const HRESULT queried = implementation->enumerator->GetVideoProcessorCaps(&caps);
    if (FAILED(queried)) return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(hresult_error(
        "nv12_converter.rotation_query_failed", "Could not query video-processor rotation support.", queried));
    if ((caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) == 0)
      return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(converter_error(
          "nv12_converter.rotation_unsupported", "The selected GPU cannot rotate this display capture."));
    const auto rotation = static_cast<D3D11_VIDEO_PROCESSOR_ROTATION>(config.rotation - DXGI_MODE_ROTATION_IDENTITY);
    implementation->video_context->VideoProcessorSetStreamRotation(implementation->processor.Get(), 0, TRUE, rotation);
    BOOL enabled = FALSE;
    D3D11_VIDEO_PROCESSOR_ROTATION actual = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
    implementation->video_context->VideoProcessorGetStreamRotation(implementation->processor.Get(), 0, &enabled, &actual);
    if (!enabled || actual != rotation)
      return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(converter_error(
          "nv12_converter.rotation_rejected", "The GPU did not retain the requested desktop rotation."));
  }

  const RECT source_rect{0, 0, static_cast<LONG>(config.input_width),
                         static_cast<LONG>(config.input_height)};
  const RECT destination_rect{0, 0, static_cast<LONG>(config.output_width),
                              static_cast<LONG>(config.output_height)};
  implementation->video_context->VideoProcessorSetStreamSourceRect(
      implementation->processor.Get(), 0, TRUE, &source_rect);
  implementation->video_context->VideoProcessorSetStreamDestRect(
      implementation->processor.Get(), 0, TRUE, &destination_rect);
  implementation->video_context->VideoProcessorSetOutputTargetRect(
      implementation->processor.Get(), TRUE, &destination_rect);

  DXGI_COLOR_SPACE_TYPE actual_input_color_space = DXGI_COLOR_SPACE_CUSTOM;
  DXGI_COLOR_SPACE_TYPE actual_output_color_space = DXGI_COLOR_SPACE_CUSTOM;
  BOOL actual_auto_processing = TRUE;
  implementation->video_context->VideoProcessorGetStreamColorSpace1(
      implementation->processor.Get(), 0, &actual_input_color_space);
  implementation->video_context->VideoProcessorGetOutputColorSpace1(
      implementation->processor.Get(), &actual_output_color_space);
  implementation->video_context->VideoProcessorGetStreamAutoProcessingMode(
      implementation->processor.Get(), 0, &actual_auto_processing);
  if (actual_input_color_space != kInputColorSpace ||
      actual_output_color_space != kOutputColorSpace || actual_auto_processing != FALSE) {
    return Result<std::unique_ptr<D3d11Nv12Converter>>::failure(converter_error(
        "nv12_converter.processor_configuration_rejected",
        "The video processor did not retain the required color or auto-processing state."));
  }

  auto converter = std::unique_ptr<D3d11Nv12Converter>(
      new D3d11Nv12Converter(std::move(implementation)));
  return Result<std::unique_ptr<D3d11Nv12Converter>>::success(std::move(converter));
}

Result<ConvertedVideoFrame> D3d11Nv12Converter::convert(
    const CapturedVideoFrame& frame) {
  return convert_impl(frame, std::nullopt);
}

Result<ConvertedVideoFrame> D3d11Nv12Converter::convert_cached(
    const CapturedVideoFrame& frame, std::uint64_t content_revision) {
  return convert_impl(frame, content_revision);
}

Result<ConvertedVideoFrame> D3d11Nv12Converter::convert_impl(
    const CapturedVideoFrame& frame, std::optional<std::uint64_t> revision) {
  if (frame.texture == nullptr || frame.lifetime == nullptr) {
    return Result<ConvertedVideoFrame>::failure(converter_error(
        "nv12_converter.invalid_input",
        "The captured frame texture and Desktop Duplication lease are required."));
  }
  auto& state = *implementation_;
  if (frame.width != state.config.input_width || frame.height != state.config.input_height) {
    return Result<ConvertedVideoFrame>::failure(converter_error(
        "nv12_converter.input_dimensions_changed",
        "The BGRA frame dimensions differ from the reusable processor configuration."));
  }
  const std::lock_guard conversion_lock(state.conversion_mutex);
  // Descriptors and device identity are immutable. Retain one input view; the
  // production pipeline writes into one persistent BGRA texture.
  if (state.input_texture.Get() != frame.texture.Get()) {
    D3D11_TEXTURE2D_DESC description{};
    frame.texture->GetDesc(&description);
    if (description.Width != frame.width || description.Height != frame.height ||
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0 ||
        description.SampleDesc.Count != 1) {
      return Result<ConvertedVideoFrame>::failure(converter_error(
          "nv12_converter.invalid_input_texture",
          "The input must be a same-size GPU-only, single-sample BGRA8 texture."));
    }
    Microsoft::WRL::ComPtr<ID3D11Device> input_device;
    frame.texture->GetDevice(&input_device);
    if (!same_com_identity(input_device.Get(), state.device.Get())) {
      return Result<ConvertedVideoFrame>::failure(converter_error(
          "nv12_converter.input_device_mismatch",
          "The captured BGRA texture belongs to a different D3D11 device."));
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view_description{};
    view_description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    const HRESULT result = state.video_device->CreateVideoProcessorInputView(
        frame.texture.Get(), state.enumerator.Get(), &view_description, &input_view);
    if (FAILED(result)) {
      return Result<ConvertedVideoFrame>::failure(hresult_error(
          "nv12_converter.input_view_creation_failed",
          "Could not bind the captured BGRA texture as video-processor input.", result));
    }
    state.input_view = std::move(input_view);
    state.input_texture = frame.texture;
    ++state.metrics.input_views_created;
    // Revisions apply to one input texture, including when its COM address
    // would otherwise be reused. Our retained reference prevents that reuse.
    for (auto& slot : state.output_pool->slots) slot.revision.reset();
  }

  // Preserve the capture lease through GPU submission. Output ownership is
  // separate: only an unleased NV12 surface can be handed to another encode.
  const auto input_lifetime = frame.lifetime;
  auto output_result = acquire_output(state.output_pool);
  if (!output_result.is_success()) {
    return Result<ConvertedVideoFrame>::failure(std::move(output_result).error());
  }
  auto output = std::move(output_result).value();
  auto& slot = state.output_pool->slots[output.index];
  if (revision.has_value() && slot.revision == revision) {
    ++state.metrics.cached_conversions;
    return Result<ConvertedVideoFrame>::success(
        ConvertedVideoFrame(std::move(output.texture), std::move(output.lease)));
  }
  slot.revision.reset();
  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = state.input_view.Get();
  const HRESULT result = state.video_context->VideoProcessorBlt(
      state.processor.Get(), output.view.Get(), 0, 1, &stream);
  if (FAILED(result)) {
    return Result<ConvertedVideoFrame>::failure(hresult_error(
        "nv12_converter.video_process_failed",
        "The GPU rejected BGRA-to-NV12 video processing submission.", result));
  }
  ++state.metrics.processing_submissions;
  slot.revision = revision;
  return Result<ConvertedVideoFrame>::success(
      ConvertedVideoFrame(std::move(output.texture), std::move(output.lease)));
}
D3d11Nv12RuntimeMetrics D3d11Nv12Converter::metrics() {
  const std::lock_guard conversion_lock(implementation_->conversion_mutex);
  return implementation_->metrics;
}

D3d11Nv12ProcessorState D3d11Nv12Converter::processor_state() {
  const std::lock_guard conversion_lock(implementation_->conversion_mutex);
  D3d11Nv12ProcessorState state{DXGI_COLOR_SPACE_CUSTOM, DXGI_COLOR_SPACE_CUSTOM, true};
  BOOL auto_processing_enabled = TRUE;
  implementation_->video_context->VideoProcessorGetStreamColorSpace1(
      implementation_->processor.Get(), 0, &state.input_color_space);
  implementation_->video_context->VideoProcessorGetOutputColorSpace1(
      implementation_->processor.Get(), &state.output_color_space);
  implementation_->video_context->VideoProcessorGetStreamAutoProcessingMode(
      implementation_->processor.Get(), 0, &auto_processing_enabled);
  state.auto_processing_enabled = auto_processing_enabled != FALSE;
  return state;
}

const D3d11Nv12ConverterConfig& D3d11Nv12Converter::config() const noexcept {
  return implementation_->config;
}

}  // namespace rebelliocap
