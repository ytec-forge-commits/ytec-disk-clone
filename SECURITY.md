# Security Policy

Y-TEC Tsumugi Drive performs safety-critical disk operations. Please report a
possible vulnerability without testing it against production data, a physical
disk that contains data, or another person's system.

## Supported versions

There is currently no supported stable release. This personally developed
open-source project publishes the `1.0.0-internal-beta` pre-release validation
line. It is not company-internal software. Security fixes are applied to the
current `main` branch. Historical development snapshots and unofficial
third-party builds are not supported.

## Private reporting

Open the repository's **Security** tab and use **Report a vulnerability**
through GitHub Private Vulnerability Reporting. Do not open a public Issue for
a vulnerability or include technical details, proof of concept, logs, or
affected data in a public discussion.

## Never include in a report

- a real disk or partition image;
- a complete disk serial or unredacted device inventory;
- a BitLocker recovery key, password, derived key, product key, API key,
  credential, cookie, private key, or token;
- personal, customer, company, or production data;
- an unredacted support ZIP or log containing private paths or identifiers.

Use synthetic identifiers and the smallest safe reproduction possible.

## Helpful report contents

- affected commit, version, and Windows/WinPE environment;
- a high-level description of the affected component and expected safety
  boundary;
- reproduction using synthetic input or an in-memory mock;
- whether the issue could select the wrong target, write to a source, accept an
  untrusted image, bypass verification, expose sensitive data, or execute an
  untrusted command;
- suggested mitigation, if known.

Do not continue testing after observing unexpected physical I/O or a possible
write to a protected source. Stop, preserve only redacted non-sensitive
evidence, and report the boundary failure.

## Response and disclosure

This community project does not promise a response-time SLA or bug bounty. The
maintainers will acknowledge and triage reports as capacity allows, coordinate
a fix and regression test, and agree on disclosure timing before publishing
technical details. Please do not disclose an unresolved vulnerability publicly
without allowing a reasonable opportunity to investigate it.
