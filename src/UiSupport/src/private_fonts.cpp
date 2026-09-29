#include "ytec/uisupport/private_fonts.h"

#include "ytec/uisupport/font_resource_ids.h"

#include <array>

namespace ytec::uisupport {
namespace {

constexpr wchar_t kRegularFace[] = L"LINE Seed JP App_TTF Regular";
constexpr wchar_t kBoldFace[] = L"LINE Seed JP App_TTF Bold";
constexpr wchar_t kFallbackFace[] = L"Yu Gothic UI";

}  // namespace

PrivateFontCollection::~PrivateFontCollection() {
  reset();
}

bool PrivateFontCollection::load_line_seed_jp(HMODULE module) noexcept {
  reset();
  if (module == nullptr) {
    return false;
  }

  constexpr std::array<int, 2> resource_ids{
      IDR_LINE_SEED_JP_APP_REGULAR,
      IDR_LINE_SEED_JP_APP_BOLD};
  for (const int resource_id : resource_ids) {
    const HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (resource == nullptr) {
      reset();
      return false;
    }
    const DWORD size = SizeofResource(module, resource);
    const HGLOBAL loaded_resource = LoadResource(module, resource);
    const void* bytes = loaded_resource == nullptr
        ? nullptr
        : LockResource(loaded_resource);
    if (size == 0 || bytes == nullptr) {
      reset();
      return false;
    }

    DWORD font_count{};
    const HANDLE font = AddFontMemResourceEx(
        const_cast<void*>(bytes), size, nullptr, &font_count);
    if (font == nullptr || font_count == 0) {
      reset();
      return false;
    }
    resources_[loaded_++] = font;
  }
  return true;
}

bool PrivateFontCollection::line_seed_jp_available() const noexcept {
  return loaded_ == resources_.size();
}

const wchar_t* PrivateFontCollection::regular_face() const noexcept {
  return line_seed_jp_available() ? kRegularFace : kFallbackFace;
}

const wchar_t* PrivateFontCollection::bold_face() const noexcept {
  return line_seed_jp_available() ? kBoldFace : kFallbackFace;
}

void PrivateFontCollection::reset() noexcept {
  while (loaded_ > 0) {
    --loaded_;
    if (resources_[loaded_] != nullptr) {
      static_cast<void>(RemoveFontMemResourceEx(resources_[loaded_]));
      resources_[loaded_] = nullptr;
    }
  }
}

}  // namespace ytec::uisupport
