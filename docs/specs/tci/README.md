# TCI Protocol 2.1 specification

`TCI Protocol 2.1.pdf` is Expert Electronics' published specification of the
Transceiver Control Interface. AetherSDR's TCI server
(`src/core/TciProtocol.cpp`, `src/core/TciServer.cpp`) implements it, and it is
the authority for command names, argument order, units and ranges. Cite it as
`docs/specs/tci/TCI Protocol 2.1.pdf` and the command's section, for example
§AGC_GAIN.

| | |
|---|---|
| Source | https://github.com/ExpertSDR3/TCI |
| Upstream commit | `6cbccdd7004858152b4b5b463f9b087432d81a21` (2026-10-22) |
| File | `TCI Protocol 2.1.pdf`, unmodified |
| SHA-256 | `e60833985619ca6a1c7b5c4c48bdbff59e694924589c238d06557e011e18c074` |
| License | MIT, Copyright (c) 2021 Expert Electronics; see `LICENSE` in this folder |

Upstream also publishes a Russian edition (`TCI Protocol_RU 2.1.pdf`), which is
not copied here.

Names AetherSDR accepts or sends that are not in 2.1, such as the
`rx_ctun_ex` command and the `dsb` modulation, are extensions.
`rx_ctun_ex` is described in `docs/user/docs/tci-server.md`.

To update: download the new PDF from the upstream repository, replace the file,
and update the commit, SHA-256 and version in this table and in
`THIRD_PARTY_LICENSES`.
