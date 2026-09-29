#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ytec::clonecore {

// Values which must never be copied verbatim into the product's main log.
// Callers that already know the value's meaning should prefer
// minimize_main_log_value() over embedding it in an unstructured message.
enum class MainLogPrivateValueKind {
  secret,
  absolute_path,
  document_name,
  disk_serial,
  device_instance_id,
};

// Produces a log-safe representation. Secrets, paths and document names are
// removed. Disk identifiers use a domain-separated SHA-256 token; a disk
// serial may additionally expose only its final four characters when the
// original value is longer than eight characters. A hashing failure produces a
// fixed marker; an allocation failure produces an empty string. Neither case
// copies the original value, so callers fail closed.
[[nodiscard]] std::wstring minimize_main_log_value(
    MainLogPrivateValueKind kind,
    std::wstring_view value) noexcept;

// Defense-in-depth for existing unstructured log calls. Credential markers,
// absolute paths, document-like names and common device-instance forms are
// removed before a Logger sink observes the record. Records containing raw
// identifier assignments receive a fixed marker; only the structured API
// emits a hashed/minimized identifier. This is not a substitute for the
// structured API when a caller already has a private value.
// Output is always capped at the product main-log limit of 8192 characters,
// even if a caller requests a larger bound.
[[nodiscard]] std::wstring sanitize_main_log_message(
    std::wstring_view message,
    std::size_t maximum_characters = 8192U) noexcept;

}  // namespace ytec::clonecore
