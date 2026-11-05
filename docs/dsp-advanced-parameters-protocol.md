# Advanced radio DSP parameters

Related: #6615, parent integration tracker #6437, parameter resets #6509,
Auto AGC-T #6508. These are desktop RX requests, not daemon methods or TX grants.

## Authority

The published [FlexLib API 4.3.16.42034](https://edge.flexradio.com/www/offload/20261012095725/FlexLib_API_v4.3.16.zip)
(SHA-256 `60978db51dac6ea781899dd80832ad4e7876ce96ed72532b57106376fc26463f`)
provides setters and parsers in `FlexLib/Slice.cs`. The
[official DSP Controls Panel reference](https://helpdesk.flexradio.com/hc/en-us/articles/56306070856475-How-to-Use-the-DSP-Controls-Panel)
supplies ANFT's bounds, hardware/mode restrictions and retained entitlement.
FlexLib's ANFT integer setters alone do not establish bounds.

Every command uses `slice set <owned-id> <key>=<value>`:

| Parameter | Command key | Status key | Valid domain | Slice.cs setter / parser lines |
| --- | --- | --- | --- | --- |
| NR delay | `nr_delay` | same | 1..512 | 1566–1590 / 3623–3634 |
| NR window | `nr_wlen` | same | 32..512, powers of two | 1592–1617 / 3649–3660 |
| NR inverse SDFT | `nr_isdft_mode` | same | one, half, zero | 1619–1639 / 3636–3647 |
| NR adaptation | `nr_adapt_mode` | same | auto, fast, slow | 1641–1659 / 3610–3621 |
| ANF delay | `anf_delay` | same | 1..512 | 1661–1685 / 2849–2860 |
| ANF window | `anf_wlen` | same | 32..512, powers of two | 1687–1712 / 2875–2886 |
| ANF inverse SDFT | `anf_isdft_mode` | same | one, half, zero | 1714–1734 / 2862–2873 |
| ANF adaptation | `anf_adapt_mode` | same | auto, fast, slow | 1736–1754 / 2836–2847 |
| NRL delay | `lms_nr_delay` | `nrl_delay` | 1..512 | 1756–1780 / 3691–3702 |
| NRL taps | `lms_nr_taps` | `nrl_filter_size` | 1..512 | 1782–1806 / 3717–3728 |
| NRL leakage | `lms_nr_leakage_level` | `nrl_leakage_level` | 0..100 | 1808–1831 / 3704–3715 |
| ANFL delay | `lms_anf_delay` | `anfl_delay` | 1..512 | 1833–1857 / 2917–2928 |
| ANFL taps | `lms_anf_taps` | `anfl_filter_size` | 1..512 | 1859–1883 / 2943–2954 |
| ANFL leakage | `lms_anf_leakage_level` | `anfl_leakage_level` | 0..100 | 1885–1908 / 2930–2941 |
| ANFT tone minimum | `anft_min_db` | same | −150..−50 dB | 1910–1922 / 2972–2983 |
| ANFT tone difference | `anft_tone_db_diff` | same | 1..60 dB | 1924–1936 / 2985–2996 |
| NRF window | `nrf_wlen` | same | 512..8192, powers of two | 1938–1963 / 3746–3757 |

## Admission and state

Support requires a completely parsed numeric firmware version >=4.3.16 and a
known platform. NRF additionally requires 8000/Aurora hardware; the other five
algorithms cover 6000-series radios, including FLEX-6700. The feature record
shares the existing reset mode and retained-entitlement classification.
`noise_reduction enabled=1` is affirmative feature evidence; an active or
expired subscription alone is not. Mapping this published feature name to the
UI's Advanced Noise Mitigation term is the same conservative inference as
[parameter resets](./dsp-parameter-reset-protocol.md), not a proprietary UI binding.

Requests validate parameter identity and domain, current owner thread, backend,
connection, exact owned slice object, session, confirmed supported mode and the
selected parameter's observed value. Sparse observations have no defaults and
change only from validated status. Any valid report, even a repeated value,
advances an observation revision; disconnect/backend replacement clears them.
The editor captures radio, slice, session, revision and value for a deliberate
edit and again on Apply press. It consumes that context once. Rebinding,
revocation or fresh profile/reset status discards pending edits. Status cannot
send requests. A successful setter reply schedules one read-only `sub slice all`
to obtain confirmed values: FLEX-6700 4.3.16.42034 acknowledges these setters
without necessarily publishing unsolicited slice status. An error reply or
failed dispatch does not refresh or retry. Duplicate replies refresh at most
once. The queued refresh retains the original writer and requires unchanged
writer binding, backend, radio session, exact slice and slice-session generation;
it accepts intervening value reports. A stale callback has no effect.
There is no AppSettings or host state recall for these values.

## Legacy behavior

Published [FlexLib 4.2.20.41343](https://edge.flexradio.com/www/offload/20260602072117/FlexLib_API_v4.2.20.zip)
(SHA-256 `020ccfa68578570bc123f14412265f7c1d894bde2a660e2b973321b462a5fbeb`)
contains no advanced-mode setters, parsers or numeric enum mapping. #6437 has
observed numeric 4.2 status and string 4.3 status, but that does not establish
every correspondence. Numeric mode values are rejected rather than mapped by
assumption; advanced editing on old firmware remains unavailable. Existing
supported ordinary enable and level requests remain intact.

The old `Slice.cs:1286–1312` exposes `nrf_level`, without a matching status
parser. The 4.3 archive removes it entirely and the official panel identifies
NRF as having no level slider. The verified current contract refuses that
obsolete level request and disables its legacy slider with an explanation;
NRF's window is the supported advanced parameter. Older firmware keeps the
old setter. This does not claim that the old setter converges on hardware.

## Evidence limits

Socket-free tests inject production decoders, model bindings and Flex sinks.
The optional GUI test `--bridge-fixture` binds only our own local agent
automation bridge and separates request capture from injected status. It
proves client behavior, not firmware convergence. Live RX checks require the
radio lock, a captured initial state and restoration. No RF is needed.

Live RX verification on FLEX-6700 firmware 4.3.16.42034 confirmed and restored
all 16 parameters supported by that hardware, including both string-mode
families and non-power-of-two LMS taps. RX frame counters advanced throughout;
ordinary receive controls stayed unchanged, no TX was requested and the final
`transmitting` state was false. NRF's
hardware refusal was observed on the 6700; its positive path is source- and
fixture-verified, without 8000/Aurora hardware testing. Profile/reset convergence
is tested through independently injected production status, not a live recall.
