#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>

namespace ytec::uisupport {

class PrivateFontCollection final {
 public:
  PrivateFontCollection() = default;
  ~PrivateFontCollection();

  PrivateFontCollection(const PrivateFontCollection&) = delete;
  PrivateFontCollection& operator=(const PrivateFontCollection&) = delete;
  PrivateFontCollection(PrivateFontCollection&&) = delete;
  PrivateFontCollection& operator=(PrivateFontCollection&&) = delete;

  [[nodiscard]] bool load_line_seed_jp(HMODULE module) noexcept;
  [[nodiscard]] bool line_seed_jp_available() const noexcept;
  [[nodiscard]] const wchar_t* regular_face() const noexcept;
  [[nodiscard]] const wchar_t* bold_face() const noexcept;

 private:
  void reset() noexcept;

  std::array<HANDLE, 2> resources_{};
  std::size_t loaded_{};
};

}  // namespace ytec::uisupport
