#pragma once

#include "ytec/operationcore/resume_slot.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ytec::operationcore {

// The slot envelope includes the bounded checkpoint plus the immutable
// source/target/output and optional owned-partial binding.  It is deliberately
// small enough to read and authenticate in one bounded allocation.
inline constexpr std::size_t kMaximumWindowsResumeSlotBytes =
    384U * 1024U;

// Operation-specific code identifies the EXE-adjacent data directory from an
// opened storage handle.  Startup inspection and binding-checked discard may
// run before an image/source has been reselected, so separated_from_source is
// required only for create/replace mutations.  The adapter binds the first
// non-zero identity and requires the same opened-storage identity on every
// later observation or mutation performed by that adapter instance.
struct WindowsResumeDataBackingProof final {
  Sha256Digest backing_storage_identity_hash{};
  bool identity_from_open_handle{};
  bool separated_from_source{};
};

using WindowsResumeDataBackingProbe = std::function<
    clonecore::Result<WindowsResumeDataBackingProof>(
        const std::wstring&,
        const std::optional<ResumeSlotRecord>&)>;

// The path is platform-private metadata persisted inside the authenticated
// slot envelope.  ResumeSlotRecord remains path-free and keeps its existing
// engine/platform contract.
struct WindowsResumeOwnedPartial final {
  std::wstring canonical_path;
  ResumeOwnedPartialBinding binding{};
};

struct WindowsResumeOwnedObject final {
  std::wstring canonical_path;
  ResumeOwnedObjectBinding binding{};
};

// Opens a caller-selected .partial and derives its binding from the regular,
// non-reparse, single-link file object.  The caller supplies operation and
// engine identities; this helper supplies only the opened file-object hash.
[[nodiscard]] clonecore::Result<WindowsResumeOwnedPartial>
bind_windows_resume_owned_partial(
    const std::wstring& path,
    const OperationId& operation_id,
    const ResumeIdentityBinding& identities);

// Schema-v3 counterpart of bind_windows_resume_owned_partial(). The role is
// included in the file-object digest domain so one file cannot be rebound as a
// different transaction object.
[[nodiscard]] clonecore::Result<WindowsResumeOwnedObject>
bind_windows_resume_owned_object(
    const std::wstring& path,
    ResumeOwnedObjectRole role,
    const OperationId& operation_id,
    const ResumeIdentityBinding& identities);

struct WindowsResumeSlotPlatformOptions final {
  // Must be an absolute canonical local path.  The fixed slot is derived as
  // <executable parent>\data\active.checkpoint; callers cannot select a second
  // slot or an AppData fallback.
  std::wstring executable_path;
  WindowsResumeDataBackingProbe prove_data_backing_separation;

  // Required only while creating a record that declares owned_partial.  A
  // persisted slot carries its authenticated partial path and can therefore
  // be inspected/discarded after process restart without this value.
  std::optional<WindowsResumeOwnedPartial> owned_partial_for_create;

  // Required while creating a schema-v3 multi-object slot. The canonical
  // vector is role-sorted and must exactly match ResumeSlotRecord::owned_objects.
  std::vector<WindowsResumeOwnedObject> owned_objects_for_create;
};

// Creates the production Win32 adapter.  The EXE parent and its existing data
// directory are verified as canonical, local, regular and reparse-free before
// the adapter is returned.  The factory never creates a directory or file.
[[nodiscard]] clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>
make_windows_resume_slot_platform(
    WindowsResumeSlotPlatformOptions options);

// Convenience production factory for the currently running executable.
[[nodiscard]] clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>
make_current_executable_windows_resume_slot_platform(
    WindowsResumeDataBackingProbe prove_data_backing_separation,
    std::optional<WindowsResumeOwnedPartial> owned_partial_for_create =
        std::nullopt,
    std::vector<WindowsResumeOwnedObject> owned_objects_for_create = {});

}  // namespace ytec::operationcore
