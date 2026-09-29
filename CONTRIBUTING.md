# Contributing to Y-TEC Tsumugi Drive

Thank you for helping improve Y-TEC Tsumugi Drive. This is a personally
developed open-source, safety-critical disk project released under the Y-TEC
name. It is not company-internal software. The current
`1.0.0-internal-beta` build is a pre-release validation candidate, not a stable
1.0.0 release.
Representative hardware acceptance is incomplete, so every contribution must
preserve the project's fail-closed behavior and state its validation limits.

日本語でのIssue／Pull Requestも歓迎します。

## Before starting

1. Read the repository `AGENTS.md`,
   [authoritative specification](DiskClone_Development_Spec.md), and
   [safety model](docs/safety-model.md).
2. Search existing Issues and Pull Requests.
3. Open an Issue before a large feature, file-format change, dependency change,
   or change to a destructive-I/O boundary.
4. Keep the change narrowly scoped. Do not combine unrelated formatting,
   dependency updates, or refactors.

Security vulnerabilities must follow [SECURITY.md](SECURITY.md), not a public
Issue containing exploit details.

## Safety and test-data rules

- Never run contribution or CI tests against a physical disk, real USB media,
  production system, or user data.
- Use synthetic buffers, in-memory adapters, temporary files, and disposable
  virtual disks created specifically for the test.
- Do not upload disk images, support archives, complete disk serials, recovery
  keys, passwords, credentials, cookies, private keys, or personal information.
- Source-disk reads and target-disk writes must remain separate by type and
  ownership. A source disk must never be opened for application write access.
- Re-identify a target by stable properties before destructive work. Disk
  numbers and drive letters alone are not identities.
- Unknown, ambiguous, unsupported, or changed state must stop before I/O.
- Failure, cancellation, partial loss, or incomplete verification must never be
  reported as success.
- Do not weaken signature checks, BitLocker/Secure Boot protections, command
  allowlists, or the uppercase `OK` confirmation boundary.

Physical-device, USB, real-boot, and destructive VM validation are maintainer-
controlled acceptance activities. A contributor must not perform or claim
those checks unless a maintainer explicitly coordinates the isolated test.

## Dependencies and provenance

Do not add or update a dependency in an implementation Pull Request without
prior maintainer approval. The proposal must include:

- exact name and version;
- official source and archive SHA-256;
- SPDX license identifier, copyright, and required notice text;
- purpose, link method, and whether it enters a distributed artifact;
- alternatives and a removal or rollback plan.

Do not copy code, UI, text, formats, icons, or other assets from proprietary or
unknown sources. Preserve all applicable third-party licenses and notices.
Microsoft WinPE/ADK/WIM/ISO/CAB/EXE/DLL payloads must not be committed or
included in a product package.

## Build and nonphysical validation

The supported toolchain is Windows x64, CMake 3.25+, MSVC C++ Build Tools,
Ninja, and PowerShell.

```powershell
. .\scripts\Enter-MsvcEnvironment.ps1
cmake --preset msvc-x64
cmake --build --preset msvc-x64 -- -j 2
ctest --preset msvc-x64 --output-on-failure
```

Before requesting final review, run the full nonphysical gate when the change
and local environment allow it:

```powershell
./scripts/ci.ps1
```

If a check was not run, say so and explain why. Do not describe an old run or a
different build tree as evidence for the current commit.

## Pull Request checklist

- Explain the problem, the bounded solution, and unchanged behavior.
- List changed files and any intentional deletions.
- Add or update tests for success, boundary, cancellation, and injected failure
  paths as appropriate.
- Report exact build, test, static-analysis, and packaging commands run.
- Separate verified results from VM, physical-device, and real-boot work that
  remains untested.
- Update user, architecture, safety, format, license, and SBOM documentation
  when the change affects them.
- Confirm that no secret, personal data, generated build output, Microsoft
  payload, or unapproved third-party material is included.

## Contribution license

The project's Y-TEC-owned code is licensed under
[Apache License 2.0](LICENSE). By submitting a contribution, you represent that
you have the right to submit it and agree that it may be distributed under the
project's Apache-2.0 license. Third-party material remains under its applicable
license and must be identified explicitly.
