# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability in MasterSDR, please report it
**privately** rather than opening a public issue.

**Email:** Send details to the repository owner via GitHub private messaging,
or use [GitHub's private vulnerability reporting](https://github.com/MasterSDR/MasterSDR/security/advisories/new).

Please include:
- A description of the vulnerability
- Steps to reproduce
- Potential impact

We will acknowledge your report within 72 hours and work with you on a fix
before any public disclosure.

## Scope

MasterSDR communicates with FlexRadio hardware over local network protocols
(TCP/UDP). Security concerns include:

- Buffer overflows in VITA-49 packet parsing
- Command injection via malformed radio status messages
- Denial of service from crafted UDP packets
- Sensitive data exposure (e.g. network credentials in logs)
- SmartLink Auth0 token handling (OS keychain storage, refresh token lifecycle)
- WebSocket server endpoints (TCI, future integrations)

## Supported Versions

Only the latest version on the `main` branch receives security fixes.
