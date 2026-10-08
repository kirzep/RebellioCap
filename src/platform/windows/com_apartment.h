#pragma once

#include <Windows.h>

namespace rebelliocap {

class ComApartment {
 public:
  explicit ComApartment(DWORD model = COINIT_MULTITHREADED) noexcept
      : result_(CoInitializeEx(nullptr, model)) {}

  ~ComApartment() {
    if (SUCCEEDED(result_)) {
      CoUninitialize();
    }
  }

  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;
  ComApartment(ComApartment&&) = delete;
  ComApartment& operator=(ComApartment&&) = delete;

  [[nodiscard]] HRESULT result() const noexcept { return result_; }
  [[nodiscard]] bool initialized() const noexcept { return SUCCEEDED(result_); }

 private:
  HRESULT result_;
};

}  // namespace rebelliocap
