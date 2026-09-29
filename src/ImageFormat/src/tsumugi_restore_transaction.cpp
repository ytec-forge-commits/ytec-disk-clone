#include "ytec/imageformat/tsumugi_restore_transaction.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

clonecore::Error transaction_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool all_zero(const Sha256Digest& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

const TsumugiRestoreDiskIdentity& target_disk(
    const TsumugiRestoreTarget& target) {
  return std::visit(
      [](const auto& selected) -> const TsumugiRestoreDiskIdentity& {
        if constexpr (std::is_same_v<
                          std::decay_t<decltype(selected)>,
                          TsumugiWholeDiskRestoreTarget>) {
          return selected.disk;
        } else {
          return std::visit(
              [](const auto& individual)
                  -> const TsumugiRestoreDiskIdentity& {
                return individual.disk;
              },
              selected.target);
        }
      },
      target);
}

bool same_identity(
    const TsumugiRestoreDiskIdentity& expected,
    const TsumugiRestoreDiskIdentity& current) noexcept {
  return expected.stable_identity_hash == current.stable_identity_hash &&
      expected.disk_size == current.disk_size &&
      expected.logical_sector_size == current.logical_sector_size &&
      expected.is_running_windows_system_disk ==
          current.is_running_windows_system_disk &&
      expected.is_usb_attached == current.is_usb_attached &&
      expected.is_usb_memory == current.is_usb_memory &&
      expected.is_active_rescue_media == current.is_active_rescue_media &&
      expected.is_dynamic_disk == current.is_dynamic_disk &&
      expected.is_storage_spaces == current.is_storage_spaces &&
      expected.is_windows_software_raid ==
          current.is_windows_software_raid &&
      expected.has_unresolved_hardware_raid ==
          current.has_unresolved_hardware_raid &&
      expected.connection_instance_hash == current.connection_instance_hash;
}

bool zero_fill(const TsumugiRestoreWrite& write) noexcept {
  return write.zero_fill || write.unreadable_zero_fill;
}

}  // namespace

TsumugiBlockRestoreTransaction::TsumugiBlockRestoreTransaction(
    ITsumugiRestoreTargetSession& session,
    const std::size_t verification_block_bytes) noexcept
    : session_(&session),
      verification_block_bytes_(verification_block_bytes) {}

clonecore::Result<TsumugiRestoreDiskIdentity>
TsumugiBlockRestoreTransaction::begin(
    const TsumugiVerifiedImage& image,
    const TsumugiRestoreTarget& target,
    const TsumugiRestoreHost host) {
  if (session_ == nullptr || begun_ || committed_ ||
      verification_block_bytes_ == 0U ||
      verification_block_bytes_ > 32U * 1024U * 1024U) {
    return clonecore::Result<TsumugiRestoreDiskIdentity>::failure(
        transaction_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_STATE,
            L"Tsumugi復元トランザクション開始",
            L"対象Session、単回状態、または読戻しブロック上限が不正です"));
  }
  auto current = session_->reidentify_locked_target();
  if (!current) {
    return clonecore::Result<TsumugiRestoreDiskIdentity>::failure(
        current.error());
  }
  const auto& expected = target_disk(target);
  if (!same_identity(expected, current.value()) ||
      all_zero(current.value().stable_identity_hash) ||
      session_->size_bytes() != current.value().disk_size ||
      session_->logical_sector_size() !=
          current.value().logical_sector_size) {
    return clonecore::Result<TsumugiRestoreDiskIdentity>::failure(
        transaction_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_REINITIALIZATION_NEEDED,
            L"Tsumugiロック済み復元先再識別",
            L"ロックした対象ハンドルの安定識別、容量、セクタ、属性、またはUSB接続Sessionが計画と一致しません"));
  }
  const auto prepared = session_->prepare_layout(image, target, host);
  if (!prepared) {
    session_->abort_layout();
    return clonecore::Result<TsumugiRestoreDiskIdentity>::failure(
        prepared.error());
  }
  current_identity_ = current.value();
  begun_ = true;
  return current;
}

clonecore::Status TsumugiBlockRestoreTransaction::write_and_verify(
    const TsumugiRestoreWrite& write,
    const std::span<const std::byte> plaintext) {
  std::uint64_t end{};
  if (session_ == nullptr || !begun_ || committed_ || write.length == 0U ||
      write.stable_target_identity_hash !=
          current_identity_.stable_identity_hash ||
      write.target_offset % current_identity_.logical_sector_size != 0U ||
      write.length % current_identity_.logical_sector_size != 0U ||
      !checked_add(write.target_offset, write.length, end) ||
      end > current_identity_.disk_size ||
      (zero_fill(write) ? !plaintext.empty()
                        : plaintext.size() != write.length)) {
    return clonecore::Status::failure(transaction_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi復元書込み範囲",
        L"単回状態、対象Hash、整列、範囲、または認証済みplaintext長が不正です"));
  }

  const std::size_t block_limit = std::max<std::size_t>(
      current_identity_.logical_sector_size,
      verification_block_bytes_ -
          (verification_block_bytes_ %
           current_identity_.logical_sector_size));
  std::vector<std::byte> zeros;
  if (zero_fill(write)) {
    zeros.assign(block_limit, std::byte{0});
  }
  std::uint64_t completed = 0U;
  while (completed < write.length) {
    const auto remaining = write.length - completed;
    const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
        remaining, block_limit));
    const auto bytes = zero_fill(write)
        ? std::span<const std::byte>(zeros).first(amount)
        : plaintext.subspan(static_cast<std::size_t>(completed), amount);
    const auto written = session_->write_target(
        write.target_offset + completed, bytes);
    if (!written) {
      return written;
    }
    const auto flushed = session_->flush_target();
    if (!flushed) {
      return flushed;
    }
    auto read_back = session_->read_back(
        write.target_offset + completed, amount);
    if (!read_back) {
      return clonecore::Status::failure(read_back.error());
    }
    if (read_back.value().size() != amount ||
        !std::equal(
            bytes.begin(), bytes.end(), read_back.value().begin())) {
      return clonecore::Status::failure(transaction_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"Tsumugi復元書込み読戻し",
          L"flush後に同じ対象ハンドルから読戻したbytesが一致しません"));
    }
    completed += amount;
  }
  return clonecore::success_status();
}

clonecore::Status TsumugiBlockRestoreTransaction::commit() {
  if (session_ == nullptr || !begun_ || committed_) {
    return clonecore::Status::failure(transaction_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi復元トランザクションcommit",
        L"開始済みかつ未commitの対象Sessionがありません"));
  }
  auto status = session_->flush_target();
  if (!status) {
    return status;
  }
  status = session_->commit_layout();
  if (!status) {
    return status;
  }
  status = session_->flush_target();
  if (!status) {
    return status;
  }
  committed_ = true;
  return clonecore::success_status();
}

void TsumugiBlockRestoreTransaction::abort() noexcept {
  if (session_ != nullptr && begun_ && !committed_) {
    session_->abort_layout();
  }
}

}  // namespace ytec::imageformat
