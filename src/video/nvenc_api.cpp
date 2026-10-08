#include "video/nvenc_api.h"

#include <Windows.h>

#include <optional>
#include <string>
#include <utility>

namespace rebelliocap {
namespace {

using CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using GetMaxSupportedVersion = NVENCSTATUS(NVENCAPI*)(std::uint32_t*);

constexpr std::uint32_t packed_version(std::uint32_t major,
                                       std::uint32_t minor) noexcept {
  return (major << 4U) | (minor & 0xFU);
}

NvencApiVersion decode_version(std::uint32_t packed) noexcept {
  return NvencApiVersion{packed >> 4U, packed & 0xFU, packed};
}

Error api_error(std::string code, std::string message,
                std::optional<long> native = std::nullopt) {
  return Error{std::move(code), std::move(message), native};
}

bool required_functions_present(const NV_ENCODE_API_FUNCTION_LIST& functions) noexcept {
  return functions.nvEncOpenEncodeSessionEx != nullptr &&
         functions.nvEncGetEncodeGUIDCount != nullptr &&
         functions.nvEncGetEncodeGUIDs != nullptr &&
         functions.nvEncGetEncodeProfileGUIDCount != nullptr &&
         functions.nvEncGetEncodeProfileGUIDs != nullptr &&
         functions.nvEncGetInputFormatCount != nullptr &&
         functions.nvEncGetInputFormats != nullptr &&
         functions.nvEncGetEncodeCaps != nullptr &&
         functions.nvEncGetEncodePresetCount != nullptr &&
         functions.nvEncGetEncodePresetGUIDs != nullptr &&
         functions.nvEncGetEncodePresetConfigEx != nullptr &&
         functions.nvEncInitializeEncoder != nullptr &&
         functions.nvEncCreateBitstreamBuffer != nullptr &&
         functions.nvEncDestroyBitstreamBuffer != nullptr &&
         functions.nvEncEncodePicture != nullptr &&
         functions.nvEncLockBitstream != nullptr &&
         functions.nvEncUnlockBitstream != nullptr &&
         functions.nvEncGetSequenceParams != nullptr &&
         functions.nvEncRegisterAsyncEvent != nullptr &&
         functions.nvEncUnregisterAsyncEvent != nullptr &&
         functions.nvEncMapInputResource != nullptr &&
         functions.nvEncUnmapInputResource != nullptr &&
         functions.nvEncRegisterResource != nullptr &&
         functions.nvEncUnregisterResource != nullptr &&
         functions.nvEncDestroyEncoder != nullptr;
}

}  // namespace

struct NvencApi::Impl {
  HMODULE module{nullptr};
  NvencApiVersion maximum{};
  NV_ENCODE_API_FUNCTION_LIST functions{};

  ~Impl() {
    if (module != nullptr) {
      FreeLibrary(module);
    }
  }
};

NvencApi::NvencApi(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

NvencApi::NvencApi(NvencApi&&) noexcept = default;
NvencApi& NvencApi::operator=(NvencApi&&) noexcept = default;
NvencApi::~NvencApi() = default;

Result<NvencApi> NvencApi::load() {
  auto implementation = std::make_unique<Impl>();
  implementation->module = LoadLibraryExW(
      L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (implementation->module == nullptr) {
    return Result<NvencApi>::failure(api_error(
        "nvenc.library_unavailable",
        "The NVIDIA driver NVENC runtime nvEncodeAPI64.dll is unavailable.",
        static_cast<long>(GetLastError())));
  }

  const auto get_maximum = reinterpret_cast<GetMaxSupportedVersion>(
      GetProcAddress(implementation->module, "NvEncodeAPIGetMaxSupportedVersion"));
  const auto create_instance = reinterpret_cast<CreateInstance>(
      GetProcAddress(implementation->module, "NvEncodeAPICreateInstance"));
  if (get_maximum == nullptr || create_instance == nullptr) {
    return Result<NvencApi>::failure(api_error(
        "nvenc.driver_exports_missing",
        "The NVIDIA driver NVENC runtime omits required API entry points."));
  }

  std::uint32_t maximum = 0;
  const NVENCSTATUS version_status = get_maximum(&maximum);
  if (version_status != NV_ENC_SUCCESS) {
    return Result<NvencApi>::failure(api_error(
        "nvenc.version_query_failed",
        "The NVIDIA driver failed maximum NVENC API version negotiation.",
        static_cast<long>(version_status)));
  }
  implementation->maximum = decode_version(maximum);
  constexpr std::uint32_t required =
      packed_version(NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
  if (maximum < required) {
    return Result<NvencApi>::failure(api_error(
        "nvenc.driver_api_too_old",
        "The installed NVIDIA driver does not support required NVENC API 13.0.",
        static_cast<long>(maximum)));
  }

  implementation->functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  const NVENCSTATUS create_status = create_instance(&implementation->functions);
  if (create_status != NV_ENC_SUCCESS) {
    return Result<NvencApi>::failure(api_error(
        "nvenc.function_table_failed",
        "The NVIDIA driver rejected the NVENC API 13.0 function table.",
        static_cast<long>(create_status)));
  }
  if (!required_functions_present(implementation->functions)) {
    return Result<NvencApi>::failure(api_error(
        "nvenc.function_table_incomplete",
        "The NVIDIA driver returned an incomplete NVENC function table."));
  }

  return Result<NvencApi>::success(NvencApi(std::move(implementation)));
}

NvencApiVersion NvencApi::max_supported_version() const noexcept {
  return implementation_->maximum;
}

NvencApiVersion NvencApi::required_version() const noexcept {
  constexpr std::uint32_t required =
      packed_version(NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
  return decode_version(required);
}

bool NvencApi::has_required_functions() const noexcept {
  return required_functions_present(implementation_->functions);
}

const NV_ENCODE_API_FUNCTION_LIST& NvencApi::functions() const noexcept {
  return implementation_->functions;
}

}  // namespace rebelliocap
