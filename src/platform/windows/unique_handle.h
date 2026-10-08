#pragma once

#include <utility>

#include <Windows.h>

namespace rebelliocap {

class UniqueHandle {
 public:
  UniqueHandle() noexcept = default;
  explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

  ~UniqueHandle() { reset(); }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.handle_, nullptr));
    }
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] explicit operator bool() const noexcept { return is_valid(handle_); }

  [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

  void reset(HANDLE handle = nullptr) noexcept {
    if (is_valid(handle_)) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

 private:
  static bool is_valid(HANDLE handle) noexcept {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
  }

  HANDLE handle_ = nullptr;
};

}  // namespace rebelliocap
