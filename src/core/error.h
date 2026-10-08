#pragma once

#include <optional>
#include <string>

namespace rebelliocap {

struct Error {
  std::string code;
  std::string message;
  std::optional<long> hresult;
};

}  // namespace rebelliocap
