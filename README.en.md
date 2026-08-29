# Y-TEC Tsumugi Drive

[日本語](README.md) | English

Y-TEC Tsumugi Drive is a Windows x64 native application for safety-first disk
cloning, image creation and restore, and boot repair on Windows and Windows PE.

> **This repository contains the `1.0.0-internal-beta` pre-release validation
> candidate, not the stable 1.0.0 release.** Representative hardware acceptance and the
> final distribution audit are incomplete. Do not use this build with important
> data, production systems, or as your only backup.

This is a personally developed open-source project released under the Y-TEC
name. It is not an internal company, employer, or corporate software project.
`internal-beta` is the current development identifier and does not describe the
project's ownership or intended publication model.

The authoritative product specification is currently written in Japanese in
[DiskClone_Development_Spec.md](DiskClone_Development_Spec.md). Historical
phase and handover documents are retained as engineering evidence; they do not
mean that the current product path is complete.
The similarly named `DiskClone_Development_Spec.docx` is the current v2.0
viewing document. The historical v1.0 snapshot is retained at
`docs/archive/DiskClone_Development_Spec-v1.0-20260729.docx`.

## Intended 1.0 scope

- Direct system-disk cloning from a running Windows installation through VSS
- Direct cloning, imaging, restore, and boot repair from Windows PE
- Normal migration, shrink migration, MBR preservation, and MBR-to-GPT paths
- One bounded `.tsumugi` v1 image format for system and data disks
- Stable source and target identity checks before every destructive boundary
- Explicit, user-initiated acquisition of verified Microsoft ADK/WinPE packages
- One resumable operation checkpoint and a rescue mode that reports data loss

Some foundations and product paths are implemented and covered by synthetic
tests, but the complete mode matrix, clean-VM validation, UI/DPI matrix,
representative hardware, real USB media, and real-boot acceptance are not all
complete. See [implementation status](docs/implementation-status.md) and the
[requirements traceability matrix](docs/requirements-traceability.md).

## Safety model

Destructive operations follow one common sequence:

```text
OperationPlan
  -> read-only preflight
  -> open source and target sessions
  -> re-identify stable devices
  -> show the target summary and require uppercase OK
  -> mark the target incomplete
  -> execute
  -> flush and read back
  -> commit completion metadata last
  -> OperationResult
```

- Source readers and target writers are separate types.
- A disk number or drive letter is never sufficient identity.
- Unknown layouts, filesystems, encryption states, or identifiers fail closed.
- The Windows application does not write to the source disk, although the
  running OS and VSS can perform normal system writes. Use Windows PE when an
  OS-level read-only source is required.
- Failure, cancellation, missing data, or incomplete verification is never
  presented as success.
- Automated and pull-request tests must use synthetic, nonphysical inputs. They
  must not access a contributor's physical disks or USB media.

The project does not bypass BitLocker, Secure Boot, Windows activation, or
credentials. It has no telemetry, advertising, cloud sync, or background
network access. Product networking is limited to two explicit user actions:
verified Microsoft ADK/WinPE acquisition and a manual Y-TEC update-information
check.

## Build and test

Requirements:

- Windows 10 22H2 x64 or Windows 11 x64 development environment
- CMake 3.25 or newer
- MSVC x64 C++ Build Tools
- Ninja
- PowerShell 7 or Windows PowerShell 5.1

Initialize MSVC in the same PowerShell process, then configure, build, and run
the nonphysical test suite:

```powershell
. .\scripts\Enter-MsvcEnvironment.ps1
cmake --preset msvc-x64
cmake --build --preset msvc-x64 -- -j 2
ctest --preset msvc-x64 --output-on-failure
```

The full local gate also runs the static-analysis, AddressSanitizer, licensing,
safety-boundary, SBOM, and packaging checks:

```powershell
./scripts/ci.ps1
```

A successful build or nonphysical test run is not evidence of successful
hardware cloning, restoration, USB boot, or target-only boot.

## Open-source and release policy

Y-TEC-owned source code is licensed under the
[Apache License 2.0](LICENSE). Bundled libraries and fonts remain under their
respective licenses; see [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt),
[licenses/README.md](licenses/README.md), and [SBOM.spdx.json](SBOM.spdx.json).

Use, modification, and redistribution are permitted under those licenses.
Third-party builds and forks are not official Y-TEC releases and are not
covered by Y-TEC support or compatibility claims. Y-TEC intends to publish
only audited official Portable ZIPs on its official website; no official
prebuilt binary is currently published through GitHub Releases.

Microsoft WinPE, ADK, WIM, ISO, CAB, EXE, and DLL payloads are not included in
this repository or the product ZIP. Rescue media are created locally from the
user's properly licensed and verified Microsoft installation.

## Contributing, security, and support

- Read [CONTRIBUTING.md](CONTRIBUTING.md) before proposing a change.
- Report possible vulnerabilities through [SECURITY.md](SECURITY.md).
- See [SUPPORT.md](SUPPORT.md) for the support boundary.
- Project stewardship is described in [MAINTAINERS.md](MAINTAINERS.md).
- Participation is governed by [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md), and
  official-build branding is explained in [TRADEMARKS.md](TRADEMARKS.md).

Never post a real disk image, full disk serial, recovery key, password,
credential, unredacted diagnostic archive, or personal information in a public
Issue or Pull Request.
