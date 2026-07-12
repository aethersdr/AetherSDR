<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Clean-room provenance

## Access log

| UTC date | Access | Filesystem path | SHA-256 | Purpose |
|---|---|---|---|---|
| 2026-07-11 | Read and copied | `/Users/rfoust/.codex/worktrees/8693/AetherSDR/docs/cleanroom/dstar-thumbdv-functional-spec.md` | `93e95360b1e6dae44472791a1e22e9cc9e2c0143559f899037c8dafd36efd8e2` | Sole project-specific functional specification authorized by the commissioning instructions. |
| 2026-07-11 | Hash verification and semantic read | `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/docs/dstar-thumbdv-functional-spec.md` | `93e95360b1e6dae44472791a1e22e9cc9e2c0143559f899037c8dafd36efd8e2` | Local immutable working copy of the authorized specification. |
| 2026-07-11 | Downloaded, extracted, and visually checked | `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/references/JARL-DSTAR-STD7_0.pdf` | `fd3b45aab855824c2c55ab03e79b7891349162b3072a2c93379d7105cadccb36` | Public D-STAR air-interface authority. |
| 2026-07-11 | Downloaded, extracted, and searched | `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/references/JARL-DSTAR-shogen.pdf` | `1a316de2a439b67eeb261afa02013a6e4f135c4f06cd7643fccb115bc0a92c27` | Public English equations and CRC authority. |
| 2026-07-11 | Downloaded, extracted, and visually checked | `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/references/DVSI-USB-3000-Manual.pdf` | `560621dbd5b071e4220043c42f2ba64ebb1ae925f8220e5ff5680a87abcab1d5` | Public vendor USB/parity/rate authority. |
| 2026-07-11 | Downloaded, extracted, and visually checked | `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/references/DVSI-AMBE-3000F-Manual.pdf` | `24f66449d79c8f5e5b5f9dc11032fa8808bb63ed1082764dc323d5d11b0f53f1` | Public vendor packet/field authority; no codec implementation material used. |
| 2026-07-11 | Downloaded and read | `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/references/FlexRadio-integration-guide.md` | `bd77e6a00174391abda4807ba88888ecc3ea4b1899415c8809e066813f7ab942` | Pinned public Waveform API integration behavior. |
| 2026-07-11 | Downloaded and read | `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/references/FlexRadio-waveform_api.h` | `2f2ab995262d1239cdbfdae6cd8463d51a36b926d4bdfe0462dddd480044f450` | Pinned public callback/sample declarations. |
| 2026-07-11 | Downloaded and read | `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/references/FlexRadio-vita.h` | `17b51286b37e7314e3c1ecbac299d30f9728443cf8ec7253c6812835682d5058` | Pinned public VITA wire constants and layout. |
| 2026-07-11 | Downloaded and read | `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/references/FlexRadio-vita.c` | `dee4d952f9b68c224b2c9a21041f8416269813ca0551e657f2dc68660d642c62` | Pinned public VITA serializer behavior. |
| 2026-07-11 | Downloaded and read | `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/references/FlexRadio-radio.c` | `37d31242dfb1a8cd78eb52ff49e3968905b0c200159224325a527602af94626d` | Pinned public command correlation and named stream behavior. |
| 2026-07-11 | Tooling instruction read | `/Users/rfoust/.codex/plugins/cache/openai-primary-runtime/pdf/26.709.11516/skills/pdf/SKILL.md` | `b09cb414c60234a15599c04a502ce36fe6e9aa178aabe007e43a3346b5aab607` | Required local PDF inspection workflow; no project or protocol content. |

The source and copied hashes matched the expected SHA-256 before implementation began.

## Public references

Public references used after reviewing the sanitized specification are recorded below with URL, document revision/date, access date, locally computed SHA-256, and retained path. The pinned FlexRadio SDK was used only for its public transport contract. No public D-STAR/AMBE implementation source was accessed.

| Authority | Revision/date | URL | SHA-256 |
|---|---|---|---|
| JARL D-STAR Standard | Version 7.0, May 2025 | <https://www.jarl.com/d-star/STD7_0.pdf> | `fd3b45aab855824c2c55ab03e79b7891349162b3072a2c93379d7105cadccb36` |
| JARL English technical requirements | Undated edition; accessed 2026-07-11 | <https://www.jarl.com/d-star/shogen.pdf> | `1a316de2a439b67eeb261afa02013a6e4f135c4f06cd7643fccb115bc0a92c27` |
| DVSI USB-3000/3003/3012 manual | Version 2.6, September 2023 | <https://www.dvsinc.com/manuals/USB-3000_Manual.pdf> | `560621dbd5b071e4220043c42f2ba64ebb1ae925f8220e5ff5680a87abcab1d5` |
| DVSI AMBE-3000F manual | Version 4.0, October 2021 | <https://www.dvsinc.com/manuals/AMBE-3000F_manual.pdf> | `24f66449d79c8f5e5b5f9dc11032fa8808bb63ed1082764dc323d5d11b0f53f1` |
| FlexRadio Waveform SDK integration guide | Commit `7c717b034c7c60c883e6c48fb33934c5b7fb776d`; accessed 2026-07-11 | <https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/doc/README.md> | `bd77e6a00174391abda4807ba88888ecc3ea4b1899415c8809e066813f7ab942` |
| FlexRadio Waveform public API | Same pinned commit | <https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/include/waveform_api.h> | `2f2ab995262d1239cdbfdae6cd8463d51a36b926d4bdfe0462dddd480044f450` |
| FlexRadio VITA declarations | Same pinned commit | <https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/src/vita.h> | `17b51286b37e7314e3c1ecbac299d30f9728443cf8ec7253c6812835682d5058` |
| FlexRadio VITA transport | Same pinned commit | <https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/src/vita.c> | `dee4d952f9b68c224b2c9a21041f8416269813ca0551e657f2dc68660d642c62` |
| FlexRadio command transport | Same pinned commit | <https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/src/radio.c> | `37d31242dfb1a8cd78eb52ff49e3968905b0c200159224325a527602af94626d` |
| GNU GPL version 3 text | Version 3, 29 June 2007; retrieved 2026-07-11 | <https://www.gnu.org/licenses/gpl-3.0.txt> | `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986` |

The public reference PDFs remain in `work/references` outside the deliverable because their publishers retain their own distribution terms. Derived renderings used for visual verification and actually opened were:

| Path | SHA-256 |
|---|---|
| `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/pdfs/jarl/page-22.png` | `24d6a71d92a1a1b69c57bfe59d51b1e69a02736ae32cbf32465b64f33c424929` |
| `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/pdfs/jarl/key-84.png` | `de27a60021c3022c7973d06312eea8cdd11b68ddabc79d15ae3ddb0b62518342` |
| `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/pdfs/usb/page-51.png` | `492d6b88ee01f8f3d88fbde9d1d7f4816ef44fd6a2bab9d9dcdc74e8e8e45a86` |
| `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/pdfs/usb/key-39.png` | `295690b26da04a4d45ad7bcbfbef7494bc3af787945115f4fcfe05734c5e11bb` |
| `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/pdfs/usb/key-40.png` | `f81c9f3f1a56b9f81d6615353fc43077a103e14177c46872b4f2ae864e4b5d56` |
| `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/pdfs/ambe/page-050.png` | `080affc91ffcf4c9a2a530f45d31d9df107aa4e373d5e5df61dfcb8240bbf50a` |
| `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/pdfs/ambe/key-070.png` | `3d67d8336d3e9dfba3256547518e87f01915dbdf57c05aa066926e133d8df334` |
| `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/work/pdfs/ambe/field-104.png` | `9e87011e828a91ac41c2bea723366017b5baedcb1c4e79f20b1bdf3709c9a887` |

This access log enumerates discretionary implementation inputs. Self-authored project files, generated build products, compiler-provided headers/libraries, and the executables used to build/test are not external implementation inputs and are covered by the reproducible commands in `TESTING.md` rather than repeated as provenance rows.

## Clean-room synchronization-policy continuation

On 2026-07-11 the isolated team extended only this standalone artifact for an opt-in receive synchronization policy. No new external authority, capture, repository, Git object, implementation source, or prior conversation was opened. The change uses the already-recorded sanitized C11 observation only as an uncertainty label; the 24-bit pattern and 21-frame cadence remain the previously recorded JARL-standard inputs.

The discretionary self-authored paths opened for this continuation were:

- `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/include/crdv.h`
- `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/src/air.c`
- `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/tests/test_main.c`
- `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/tests/quarantined_acceptance.c`
- `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/CMakeLists.txt`
- `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/README.md`
- `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/INTEGRATION_CONTRACT.md`
- `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/TESTING.md`
- `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/TEST_RESULTS.md`
- `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/CLEANROOM_PROVENANCE.md`
- `/Users/rfoust/Documents/Codex/2026-07-11/aether-dstar-cleanroom-implementation/outputs/dstar-thumbdv-cleanroom/CONTENT_SHA256.txt`

Generated build paths under `/tmp/crdv-sync-build` and `work/` were implementation outputs, not source inputs. Their commands and results are recorded in `TEST_RESULTS.md`; final deliverable hashes are recorded in `CONTENT_SHA256.txt`.

## Clean-room JARL 7.0 mini-header correction

On 2026-07-11 the isolated correction implementer read only this standalone artifact and the already-linked official JARL D-STAR Standard 7.0 at <https://www.jarl.com/d-star/STD7_0.pdf>. Section 6.3 on PDF page 64 labels the four 20-character message blocks `0x40`, `0x41`, `0x42`, and `0x43`; the implementation, copied specification, vectors, contracts, and acceptance records were corrected to make that range normative for both transmission and reception and to reject `0x44`.

No AetherSDR checkout or worktree, Git object or history, `third_party/smartsdr-dsp`, source-analysis conversation, prior-run memory, or D-STAR/AMBE implementation was opened for this correction. Compiler, sanitizer, analyzer, CMake, CTest, and container tooling were used only to build and verify the independently authored artifact.

## Post-delivery preamble-qualified acquisition correction

On 2026-07-11, integration testing found that searching for the isolated
15-bit frame-sync pattern admitted noise false locks before protected-header
recovery. The correction in `include/crdv.h`, `src/air.c`, and
`tests/test_main.c` was authored from the already-recorded JARL D-STAR Standard
7.0 section 4.1.1 transmission order: at least 64 alternating bit-sync bits,
then the exact 15-bit frame sync. Initial acquisition now examines the final 16
alternating bits and permits at most two qualifier errors before requiring the
unchanged exact frame sync.

This is a post-delivery integration correction and is not represented as part
of the isolated implementation session described above. Its protocol input was
the published JARL standard already listed in this document; no AMBE codec
implementation, proprietary binary, decompiler output, or external D-STAR
implementation source was used. The functional specification, integration
contract, deterministic tests, verification record, and content manifest were
updated together so the current shipped artifact remains mechanically
auditable.

## Attestation

The implementation team did not access AetherSDR source, Git metadata or history, `third_party/smartsdr-dsp`, the source-analysis conversation, or any implementation from dimant/dstardocs, md380emu, mbevocoder, or other copyleft D-STAR/AMBE source. The hardware remains solely responsible for AMBE encode and decode.

No path below `/Users/rfoust/.codex/worktrees` other than the exact specification path in the first row was listed, traversed, searched, or read. No Git command was run. No memory or prior-run project context was used. No file in an AetherSDR repository was modified.
