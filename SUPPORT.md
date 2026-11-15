# Getting Help with AetherSDR

AetherSDR is volunteer-maintained open-source software. There is no SLA
and no guaranteed response time — but bug reports with attached support
bundles get priority, and the project's AetherClaude orchestrator
auto-triages every new issue and PR within minutes.

## Where to go

- **General questions / discussion:** GitHub Discussions —
  https://github.com/aethersdr/AetherSDR/discussions
- **Bug reports:** file an issue with the bug-report template.
  The quickest path: in the running app, **Help → Generate Support
  Bundle** and attach the resulting .zip to the issue. The bundle
  includes redacted logs, settings, and a snapshot of the radio state
  AetherClaude needs to triage.
- **Feature requests:** file an issue with the feature-request template.
- **Security issues:** **do not** open a public issue. See
  [`SECURITY.md`](SECURITY.md) for the private-disclosure process via
  GitHub Security Advisories.

## Real-time chat

Project lead (Jeremy KK7GWY) operates from DN18 in the US Pacific
Northwest. For real-time questions, ping in the GitHub Discussions
thread relevant to your topic and you'll usually get a same-day reply.

## Commercial / consulting

Email the project lead at `kk7gwy@aethersdr.com` — happy to discuss
custom integration work, sponsored development of a feature you need,
or training on the codebase.

## What gets you the fastest fix

1. A reproducible bug report on a current release (latest tag).
2. The support bundle from the app at the moment the bug occurred.
3. Hardware + firmware versions (visible in the app's title bar and
   About dialog).
4. Whether the bug also reproduces with the official SmartSDR client —
   helps narrow protocol vs. AetherSDR-specific issues.

If you can give us all four, the AetherClaude orchestrator can usually
draft a candidate fix before a human looks at the issue.