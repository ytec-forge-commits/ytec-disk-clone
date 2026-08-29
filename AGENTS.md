# Project Safety and Licensing Rules

- The specification document is the highest-priority project requirement.
- Never write to a source disk.
- Never implement destructive disk I/O without explicit task scope and tests.
- Fail closed on unknown disk layouts, filesystems, encryption, or identifiers.
- Do not add kernel drivers, telemetry, general-purpose networking, license-bypass, or BitLocker-bypass features.
- Network access is limited to two explicit, user-initiated product flows: consented Microsoft ADK/WinPE acquisition from pinned official Microsoft endpoints, and the manual Y-TEC update-information check defined by specification v2.0. There is no background network access.
- Do not copy or redistribute Microsoft WinPE/ADK/WIM/ISO/EXE/DLL files.
- Original Y-TEC project source is licensed under Apache-2.0. Preserve the
  repository `LICENSE`, `NOTICE`, source notices, and the Apache-2.0 rights to
  use, modify, and redistribute source and object forms.
- This is a personally developed open-source project released by the user
  under the Y-TEC name. It is not an internal company, employer, or corporate
  software project. Use "公開前リリース候補" or "個人開発OSS" for the
  pre-release stage; do not describe it as company-internal software.
- Official Y-TEC branding and official-build status are separate from the
  source license. Do not describe third-party builds as official, and preserve
  `TRADEMARKS.md`; do not use branding rules to restrict Apache-2.0 rights.
- Portable and WinPE deliverables that contain project binaries or scripts must
  include the project `LICENSE` and `NOTICE` alongside third-party notices.
- Rescue media must be built locally from an installed ADK/WinPE add-on. When it is missing, the application may obtain and install the pinned Microsoft packages only after displaying the official source, EULA, acquired components, and receiving explicit consent; signature, version, and SHA-256 verification are mandatory.
- Do not add GPL, AGPL, SSPL, LGPL, MPL, Commons Clause, BSL, or unknown-license dependencies.
- Any new dependency requires human approval and license documentation. The approved v2.0 baseline adds only the Argon2 reference implementation 20190702 under Apache-2.0; further dependencies still require separate approval.
- Do not imitate proprietary cloning products or copy third-party code without provenance.
- Maintain THIRD-PARTY-NOTICES.txt and SBOM.spdx.json.
- Use stable disk identity checks; never trust disk number alone.
- After bringing a freshly cloned target online, treat Volume GUID publication
  as asynchronous, especially through USB disk bridges.  Retry only a bounded
  not-found result; before every retry re-identify the stable target and
  revalidate its online/writeable/fixed state and unchanged partition layout.
  Ambiguity, unsupported content, access failure, or layout drift must still
  fail closed without mounting or running BCDBoot.
- Keep local-path volume mapping usable without elevation on normal Windows:
  prefer a read handle for the guarded WinPE fallback, but retry with a
  metadata-only handle only on access denied.  Any WinPE extent fallback must
  remain fail-closed unless independent device-number and partition-range
  queries agree and the volume GUID is unchanged.
- All image inputs are untrusted and require bounds checking.
- For CNG hashes created with a caller-owned `pbHashObject` buffer, destroy
  the `BCRYPT_HASH_HANDLE` before releasing that backing buffer.  Keep RAII
  member declaration order consistent with reverse destruction order and
  retain a repeated regression test; a single successful hash is not enough
  to detect this lifetime bug.
- Rescue image creation must never reread the failing source for the container
  pass. Rescue once into an owned, write/read-back-verified staging session,
  seal and re-identify it read-only, completely verify the final image partial,
  discard the exact owned staging object, and only then publish the final name.
- Run build, tests, static analysis, and license checks before reporting completion.
