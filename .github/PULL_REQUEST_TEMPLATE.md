## Summary

Describe the problem, the bounded change, and behavior intentionally left
unchanged.

## Safety impact

Explain any effect on source-disk protection, target identification,
destructive confirmation, image validation, command execution, privacy, or
failure handling. Write `None` only after checking each boundary.

## Validation

List the exact commands run and their results. Separate synthetic/nonphysical,
VM-only, physical-device, USB, and real-boot evidence. Do not claim a check that
was not performed.

## Dependency and license impact

List every dependency, vendored file, asset, license, notice, SBOM, or package
change. Write `None` when there is no impact.

## Checklist

- [ ] I read `AGENTS.md`, the authoritative specification, and
      `CONTRIBUTING.md`.
- [ ] The change is narrowly scoped and does not include unrelated formatting
      or dependency updates.
- [ ] Tests use synthetic/nonphysical data and do not access physical disks,
      real USB media, production systems, or user data.
- [ ] Source readers cannot acquire application write access, and destructive
      targets are re-identified by stable properties.
- [ ] Unknown, ambiguous, changed, cancelled, partial, or unverified states fail
      closed and are not reported as success.
- [ ] Relevant success, boundary, cancellation, and failure-injection tests were
      added or updated.
- [ ] Documentation, licenses, notices, provenance, SBOM, and package checks are
      updated where applicable.
- [ ] No secret, credential, recovery key, real disk image, full serial,
      personal data, generated build output, Microsoft payload, or unapproved
      third-party material is included.
- [ ] I reported every check not run and every hardware or real-boot validation
      item that remains unverified.
