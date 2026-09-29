#pragma once

#include <Windows.h>

#include <utility>

namespace ytec::clonecore {

class UniqueHandle final {
 public:
  UniqueHandle() noexcept = default;
  explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

  ~UniqueHandle() noexcept { reset(); }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

  [[nodiscard]] bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

  [[nodiscard]] HANDLE release() noexcept {
    HANDLE released = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return released;
  }

  void reset(HANDLE replacement = INVALID_HANDLE_VALUE) noexcept {
    if (valid()) {
      CloseHandle(handle_);
    }
    handle_ = replacement;
  }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

}  // namespace ytec::clonecore

