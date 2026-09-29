#include "ytec/uisupport/private_fonts.h"

#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void fail(const char* message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void check(const bool condition, const char* message) {
  if (!condition) {
    fail(message);
  }
}

void verify_face(const wchar_t* requested, const wchar_t* expected) {
  const HFONT font = CreateFontW(
      -18,
      0,
      0,
      0,
      FW_NORMAL,
      FALSE,
      FALSE,
      FALSE,
      DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY,
      DEFAULT_PITCH,
      requested);
  if (font == nullptr) {
    fail("CreateFontW failed");
  }
  const HDC dc = CreateCompatibleDC(nullptr);
  if (dc == nullptr) {
    static_cast<void>(DeleteObject(font));
    fail("CreateCompatibleDC failed");
  }
  const HGDIOBJ previous = SelectObject(dc, font);
  if (previous == nullptr || previous == HGDI_ERROR) {
    static_cast<void>(DeleteDC(dc));
    static_cast<void>(DeleteObject(font));
    fail("SelectObject failed");
  }

  wchar_t face[LF_FACESIZE]{};
  const int copied = GetTextFaceW(dc, LF_FACESIZE, face);
  if (copied <= 0) {
    static_cast<void>(SelectObject(dc, previous));
    static_cast<void>(DeleteDC(dc));
    static_cast<void>(DeleteObject(font));
    fail("GetTextFaceW failed");
  }
  const bool face_matches = std::wstring(face) == expected;

  static_cast<void>(SelectObject(dc, previous));
  static_cast<void>(DeleteDC(dc));
  static_cast<void>(DeleteObject(font));
  check(face_matches, "private font face was substituted");
}

}  // namespace

int main() {
  ytec::uisupport::PrivateFontCollection fonts;
  check(
      fonts.load_line_seed_jp(GetModuleHandleW(nullptr)),
      "embedded LINE Seed JP resources failed to load");
  check(fonts.line_seed_jp_available(), "LINE Seed JP is unavailable");
  verify_face(fonts.regular_face(), L"LINE Seed JP App_TTF Regular");
  verify_face(fonts.bold_face(), L"LINE Seed JP App_TTF Bold");
  std::cout << "ui support font tests: PASS\n";
  return 0;
}
