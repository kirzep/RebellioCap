#pragma once

#include <cstdint>
#include <memory>

#include <ffnvcodec/nvEncodeAPI.h>

#include "core/result.h"

namespace rebelliocap {

struct NvencApiVersion {
  std::uint32_t major;
  std::uint32_t minor;
  std::uint32_t packed;
};

class NvencApi {
 public:
  static Result<NvencApi> load();

  NvencApi(NvencApi&&) noexcept;
  NvencApi& operator=(NvencApi&&) noexcept;
  ~NvencApi();

  NvencApi(const NvencApi&) = delete;
  NvencApi& operator=(const NvencApi&) = delete;

  [[nodiscard]] NvencApiVersion max_supported_version() const noexcept;
  [[nodiscard]] NvencApiVersion required_version() const noexcept;
  [[nodiscard]] bool has_required_functions() const noexcept;
  [[nodiscard]] const NV_ENCODE_API_FUNCTION_LIST& functions() const noexcept;

 private:
  friend struct NvencShutdownTestAccess;
  struct Impl;

  explicit NvencApi(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> implementation_;
};

}  // namespace rebelliocap
