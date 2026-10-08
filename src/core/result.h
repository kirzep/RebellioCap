#pragma once

#include <optional>
#include <utility>
#include <variant>

#include "core/error.h"

namespace rebelliocap {

template <typename T>
class Result {
 public:
  static Result success(T value) {
    return Result(std::in_place_index<0>, std::move(value));
  }

  static Result failure(Error error) {
    return Result(std::in_place_index<1>, std::move(error));
  }

  [[nodiscard]] bool is_success() const noexcept {
    return std::holds_alternative<T>(state_);
  }

  [[nodiscard]] T& value() & { return std::get<T>(state_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(state_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(state_)); }

  [[nodiscard]] Error& error() & { return std::get<Error>(state_); }
  [[nodiscard]] const Error& error() const& { return std::get<Error>(state_); }
  [[nodiscard]] Error&& error() && { return std::get<Error>(std::move(state_)); }

 private:
  Result(std::in_place_index_t<0>, T&& value) : state_(std::move(value)) {}
  Result(std::in_place_index_t<1>, Error&& error) : state_(std::move(error)) {}

  std::variant<T, Error> state_;
};

template <>
class Result<void> {
 public:
  static Result success() { return Result(); }

  static Result failure(Error error) { return Result(std::move(error)); }

  [[nodiscard]] bool is_success() const noexcept { return !error_.has_value(); }

  [[nodiscard]] Error& error() & { return *error_; }
  [[nodiscard]] const Error& error() const& { return *error_; }
  [[nodiscard]] Error&& error() && { return std::move(*error_); }

 private:
  Result() = default;
  explicit Result(Error&& error) : error_(std::move(error)) {}

  std::optional<Error> error_;
};

}  // namespace rebelliocap
