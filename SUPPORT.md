# Support

Y-TEC Tsumugi Drive is a personally developed open-source project, not
company-internal software. It is currently the `1.0.0-internal-beta`
pre-release validation candidate, not a stable 1.0.0 release. Representative hardware, real USB,
real-boot, and final distribution acceptance are incomplete. There is no
supported stable release and no support response-time guarantee.

日本語または英語で連絡できます。

## Where to ask

- Reproducible development-preview bugs: open a GitHub Issue using the bug
  report form.
- Proposed changes: follow [CONTRIBUTING.md](CONTRIBUTING.md).
- Possible vulnerabilities: follow [SECURITY.md](SECURITY.md) and do not post
  details publicly.
- Questions about an unofficial fork or third-party build: contact that build's
  distributor. Y-TEC cannot verify or support it as an official build.

## Safe information to provide

- exact app version or commit;
- Windows version and whether the Windows or WinPE application was involved;
- affected feature and a minimal reproduction using synthetic data;
- expected and actual behavior;
- exact error code;
- tests already run and whether the result was nonphysical, VM-only, or hardware.

## Do not provide publicly

- disk or partition images;
- complete disk serials or unredacted device inventories;
- recovery keys, passwords, product keys, credentials, cookies, tokens, or
  private keys;
- personal, customer, company, or production data;
- unredacted logs, screenshots, support ZIPs, or local paths.

If unexpected physical I/O or a possible source-disk write occurs, stop using
the affected path. Do not repeatedly reproduce it on the same hardware.

## Outside the support scope

The project does not provide data-recovery services, forensic services,
Windows licensing advice, BitLocker or credential recovery, guarantees for
unsupported hardware, or support for modified third-party distributions.
Source availability and CI results do not constitute a guarantee that cloning,
restore, USB boot, or target-only boot will succeed on a particular device.
