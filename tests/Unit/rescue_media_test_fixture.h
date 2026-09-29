#pragma once

#include "ytec/imageformat/sha256.h"
#include "ytec/windowsapp/rescue_media_inspection.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rescue_media_test_fixture {

inline ytec::imageformat::Sha256Digest digest(const std::string& text) {
  const auto result = ytec::imageformat::sha256(
      std::as_bytes(std::span(text)));
  return result.has_value() ? result.value()
                            : ytec::imageformat::Sha256Digest{};
}

inline std::string digest_hex(
    const ytec::imageformat::Sha256Digest& value) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string output;
  output.reserve(value.size() * 2U);
  for (const std::byte byte : value) {
    const auto number = std::to_integer<unsigned int>(byte);
    output.push_back(kHex[(number >> 4U) & 0x0FU]);
    output.push_back(kHex[number & 0x0FU]);
  }
  return output;
}

inline ytec::windowsapp::RescueMediaFileFingerprint file(
    std::wstring path,
    const std::string& contents) {
  return {
      .relative_path = std::move(path),
      .length = static_cast<std::uint64_t>(contents.size()),
      .sha256 = digest(contents),
      .reparse_point = false,
      .hard_link_count = 1U,
  };
}

inline ytec::diskmodel::DiskInfo base_usb() {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 17U;
  disk.device_instance_id = L"USB\\VID_5954&PID_4543\\OWNED-MOCK";
  disk.model = L"Synthetic rescue USB";
  disk.size_bytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 4096U;
  disk.bus_type = L"USB";
  disk.serial_suffix = "AB12CD34";
  disk.partition_style = ytec::diskmodel::PartitionStyle::mbr;
  disk.offline = false;
  disk.read_only = false;
  disk.removable = true;
  disk.is_system_disk = false;
  return disk;
}

inline ytec::diskmodel::DiskInfo initializable_usb() {
  auto disk = base_usb();
  disk.partitions = {
      {
          .number = 1U,
          .offset_bytes = 1024ULL * 1024ULL,
          .size_bytes = 3ULL * 1024ULL * 1024ULL * 1024ULL,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x07",
      },
      {
          .number = 2U,
          .offset_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL,
          .size_bytes = disk.size_bytes -
              4ULL * 1024ULL * 1024ULL * 1024ULL,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x0C",
      },
  };
  return disk;
}

inline ytec::diskmodel::DiskInfo owned_usb() {
  auto disk = base_usb();
  constexpr std::uint64_t kOffset = 1024ULL * 1024ULL;
  constexpr std::uint64_t kBoot =
      ytec::windowsapp::kRescueUsbBootPartitionBytes;
  disk.partitions = {
      {
          .number = 1U,
          .offset_bytes = kOffset,
          .size_bytes = kBoot,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"FAT32 LBA",
          .bootable = true,
      },
      {
          .number = 2U,
          .offset_bytes = kOffset + kBoot,
          .size_bytes = disk.size_bytes - kOffset - kBoot,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"IFS",
          .bootable = false,
      },
  };
  return disk;
}

inline std::vector<std::byte> marker_bytes() {
  constexpr std::string_view kMediaId =
      "12345678-1234-1234-1234-123456789abc";
  std::vector<std::byte> bytes;
  bytes.reserve(kMediaId.size());
  for (const char character : kMediaId) {
    bytes.push_back(static_cast<std::byte>(
        static_cast<unsigned char>(character)));
  }
  return bytes;
}

inline std::vector<ytec::windowsapp::RescueMediaFileFingerprint>
boot_files() {
  return {
      file(L"sources\\boot.wim", "boot-wim"),
      file(L"bootmgr", "boot-manager"),
      file(L"EFI\\BOOT\\bootx64.efi", "efi-manager"),
      {
          .relative_path = std::wstring(
              ytec::windowsapp::kRescueUsbMarkerRelativePath),
          .length = 36U,
          .sha256 = digest(
              "12345678-1234-1234-1234-123456789abc"),
          .reparse_point = false,
          .hard_link_count = 1U,
      },
  };
}

inline std::vector<std::wstring> boot_directories() {
  return {
      L"sources",
      L"EFI",
      L"EFI\\BOOT",
      L"YtecDiskClone",
      std::wstring(ytec::windowsapp::kRescueUsbTransactionRelativePath),
  };
}

inline ytec::windowsapp::RescueMediaTreeIdentity data_identity() {
  const auto result =
      ytec::windowsapp::make_rescue_usb_private_data_tree_identity(
          {
              file(L"private-customer-name\\backup.tsumugi", "payload"),
              file(L"private-customer-name\\notes.txt", "memo"),
          },
          {L"private-customer-name"});
  return result.has_value()
      ? result.value()
      : ytec::windowsapp::RescueMediaTreeIdentity{};
}

inline ytec::windowsapp::RescueUsbOwnershipManifest owned_manifest(
    const ytec::diskmodel::DiskInfo& disk,
    const ytec::windowsapp::RescueUsbDataFileSystem file_system =
        ytec::windowsapp::RescueUsbDataFileSystem::ntfs) {
  auto files = boot_files();
  auto directories = boot_directories();
  const auto identity =
      ytec::windowsapp::make_rescue_usb_boot_tree_identity(
          files, directories);
  return {
      .schema_version =
          ytec::windowsapp::kRescueUsbOwnershipSchemaVersion,
      .purpose =
          std::wstring(ytec::windowsapp::kRescueUsbOwnershipPurpose),
      .media_id = L"12345678-1234-1234-1234-123456789abc",
      .boot_file_system = L"FAT32",
      .data_file_system = file_system,
      .boot_partition_bytes =
          ytec::windowsapp::kRescueUsbBootPartitionBytes,
      .canonical_layout =
          ytec::windowsapp::make_rescue_usb_canonical_layout(disk),
      .owned_boot_tree_identity = identity.has_value()
          ? identity.value()
          : ytec::windowsapp::RescueMediaTreeIdentity{},
      .owned_boot_files = std::move(files),
      .owned_boot_directories = std::move(directories),
  };
}

inline std::string json_escape_ascii(const std::wstring& input) {
  std::string output;
  for (const wchar_t character : input) {
    if (character == L'\\' || character == L'"') {
      output.push_back('\\');
    }
    output.push_back(static_cast<char>(character));
  }
  return output;
}

inline std::string manifest_json(
    const ytec::windowsapp::RescueUsbOwnershipManifest& manifest) {
  const auto& tree = manifest.owned_boot_tree_identity;
  std::ostringstream output;
  output << "{\"schemaVersion\":" << manifest.schema_version
         << ",\"purpose\":\"" << json_escape_ascii(manifest.purpose)
         << "\",\"mediaId\":\"" << json_escape_ascii(manifest.media_id)
         << "\",\"bootFileSystem\":\""
         << json_escape_ascii(manifest.boot_file_system)
         << "\",\"dataFileSystem\":\""
         << (manifest.data_file_system ==
                     ytec::windowsapp::RescueUsbDataFileSystem::ntfs
                 ? "NTFS"
                 : "exFAT")
         << "\",\"bootPartitionBytes\":"
         << manifest.boot_partition_bytes
         << ",\"canonicalLayout\":{\"diskStyle\":\"MBR\",\"partitions\":[";
  for (std::size_t index = 0U;
       index < manifest.canonical_layout.partitions.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    const auto& partition = manifest.canonical_layout.partitions[index];
    output << "{\"number\":" << partition.number
           << ",\"style\":\"MBR\",\"type\":\""
           << json_escape_ascii(partition.type)
           << "\",\"offsetBytes\":" << partition.offset_bytes
           << ",\"sizeBytes\":" << partition.size_bytes
           << ",\"bootable\":"
           << (partition.bootable ? "true" : "false") << '}';
  }
  output << "]},\"ownedBootTree\":{\"entryCount\":"
         << tree.entry_count << ",\"fileCount\":" << tree.file_count
         << ",\"totalPathCharacters\":" << tree.total_path_characters
         << ",\"totalLogicalBytes\":" << tree.total_logical_bytes
         << ",\"rootDigest\":\"" << digest_hex(tree.root_digest)
         << "\",\"files\":[";
  for (std::size_t index = 0U;
       index < manifest.owned_boot_files.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    const auto& entry = manifest.owned_boot_files[index];
    output << "{\"relativePath\":\""
           << json_escape_ascii(entry.relative_path)
           << "\",\"length\":" << entry.length
           << ",\"sha256\":\"" << digest_hex(entry.sha256) << "\"}";
  }
  output << "],\"directories\":[";
  for (std::size_t index = 0U;
       index < manifest.owned_boot_directories.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    output << '"'
           << json_escape_ascii(manifest.owned_boot_directories[index])
           << '"';
  }
  output << "]}}";
  return output.str();
}

inline std::vector<std::byte> bytes(const std::string& text) {
  return std::vector<std::byte>(
      reinterpret_cast<const std::byte*>(text.data()),
      reinterpret_cast<const std::byte*>(text.data() + text.size()));
}

}  // namespace rescue_media_test_fixture
