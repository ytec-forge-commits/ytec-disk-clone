# Maintainers and Project Decisions

Y-TEC Tsumugi Drive is stewarded in the `@ytec-forge-commits` organization by
the Y-TEC maintainers. GitHub CODEOWNERS currently uses the maintainer login
`@ytec-commits`; its public display name is ワイテッくん / Y-TEC kun.

## Maintainer responsibilities

Maintainers are responsible for:

- protecting the no-source-write and stable-target-identification boundaries;
- reviewing changes to destructive I/O, the `.tsumugi` format, boot repair,
  cryptography, command execution, network access, and release claims;
- approving dependency additions and keeping licenses, notices, provenance,
  and the SPDX SBOM synchronized;
- separating synthetic, VM, physical-device, USB, and real-boot evidence;
- coordinating security and conduct reports without unnecessary disclosure;
- deciding when an artifact may be described as an official Y-TEC build.

## Decision process

Routine fixes and documentation changes are decided through Pull Request
review. Significant architecture, compatibility, data-format, dependency,
security-boundary, or release-policy changes should begin with an Issue that
records the problem, alternatives, safety impact, and validation plan.

Safety and data protection take precedence over compatibility, convenience,
performance, and schedule. Unknown or disputed behavior remains fail-closed
until evidence supports a narrower decision.

When consensus cannot be reached, the maintainers make the final repository
decision and document the rationale. A maintainer with a direct conflict of
interest should not be the sole reviewer of that decision.

## Releases and support

Public source commits are not official product releases. An official Y-TEC
binary requires the project release gates, artifact audit, and clearly stated
validation scope. Unofficial forks and third-party builds may use the code
under Apache-2.0 and applicable third-party licenses, but they may not imply
Y-TEC endorsement and are outside Y-TEC support.

Maintainer participation is best effort. This document does not create a
service-level agreement, warranty, or promise of continued support.
