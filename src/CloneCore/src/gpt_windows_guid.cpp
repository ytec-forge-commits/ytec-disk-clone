#include "ytec/clonecore/gpt.h"

#include <Windows.h>
#include <bcrypt.h>

#include <cstddef>
#include <memory>

namespace ytec::clonecore {
namespace {

class WindowsGuidGenerator final : public IGuidGenerator {
 public:
  Result<GptGuid> next_guid() override {
    GptGuid guid;
    const NTSTATUS result = BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(guid.bytes.data()),
        static_cast<ULONG>(guid.bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (result < 0) {
      return Result<GptGuid>::failure(Error{
          .code = ErrorCode::internal_error,
          .native_code = static_cast<DWORD>(result),
          .operation = L"コピー先GUID生成",
          .message = L"Windows CNGによるGUID乱数生成に失敗しました",
      });
    }

    // GPT stores GUID fields in the Windows GUID byte layout. Data3 is
    // little-endian, so its UUID version nibble is the high nibble of byte 7.
    // Data4 begins at byte 8 and carries the RFC 4122 variant bits.
    guid.bytes[7] =
        (guid.bytes[7] & std::byte{0x0F}) | std::byte{0x40};
    guid.bytes[8] =
        (guid.bytes[8] & std::byte{0x3F}) | std::byte{0x80};
    return Result<GptGuid>::success(guid);
  }
};

}  // namespace

std::unique_ptr<IGuidGenerator> make_windows_guid_generator() {
  return std::make_unique<WindowsGuidGenerator>();
}

}  // namespace ytec::clonecore
