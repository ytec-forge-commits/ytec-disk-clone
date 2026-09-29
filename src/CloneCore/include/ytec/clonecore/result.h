#pragma once

#include "ytec/clonecore/error.h"

#include <stdexcept>
#include <variant>
#include <utility>

namespace ytec::clonecore {

template <typename T>
class Result final {
 public:
  [[nodiscard]] static Result success(T value) {
    return Result(std::move(value));
  }

  [[nodiscard]] static Result failure(Error error) {
    return Result(std::move(error));
  }

  [[nodiscard]] bool has_value() const noexcept {
    return std::holds_alternative<T>(storage_);
  }

  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() {
    if (!has_value()) {
      throw std::logic_error("Result does not contain a value");
    }
    return std::get<T>(storage_);
  }

  [[nodiscard]] const T& value() const {
    if (!has_value()) {
      throw std::logic_error("Result does not contain a value");
    }
    return std::get<T>(storage_);
  }

  [[nodiscard]] T&& take_value() {
    if (!has_value()) {
      throw std::logic_error("Result does not contain a value");
    }
    return std::get<T>(std::move(storage_));
  }

  [[nodiscard]] const Error& error() const {
    if (has_value()) {
      throw std::logic_error("Result does not contain an error");
    }
    return std::get<Error>(storage_);
  }

 private:
  explicit Result(T value) : storage_(std::move(value)) {}
  explicit Result(Error error) : storage_(std::move(error)) {}

  std::variant<T, Error> storage_;
};

}  // namespace ytec::clonecore

namespace ytec::clonecore {

using Status = Result<std::monostate>;

[[nodiscard]] inline Status success_status() {
  return Status::success(std::monostate{});
}

}  // namespace ytec::clonecore
