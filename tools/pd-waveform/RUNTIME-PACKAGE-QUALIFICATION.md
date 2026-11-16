# Runtime rebuild and storage qualification

Status: development. Source `6e37edc4da4732258351a7220ef0f2d13f45eeb7`.
The [machine-readable evidence](./RUNTIME-PACKAGE-QUALIFICATION.json) records
artifact hashes, every runtime member, compiler versions, checks and limits.
No radio was changed during this qualification.

## Rebuild result

| Target | Signed release bytes | Compressed payload bytes | Expanded runtime bytes |
| --- | ---: | ---: | ---: |
| Cortex-A8 ARMv7 | 9,331,383 | 9,009,224 | 22,688,874 |
| ARM64 | 9,005,703 | 8,697,852 | 22,299,642 |

Both complete runtime releases reproduced byte for byte from two independently
copied source directories with different absolute path lengths and separate
Go and Zig caches. All session-helper hashes also matched. Each release passed
the native signature/member verifier. This used Go 1.27.1, Zig 0.17.1 and the
same host, Python/compression versions and development signing key. The key
stayed in memory and subprocess input pipes; only its public half was saved.
The existing checked license bundle was reused; executables were rebuilt.

The evidence directory contains `run.py`, four build logs, the exact source
manifest and both sets of binaries. The recipe uses the vendored Go module
with `-trimpath -buildvcs=false`, `session/build.py`, the DSP/guardian builders,
`boot/build.py`, then `bundle.assemble` and `release.assemble`. It does not
substitute prebuilt binaries for either build. Source and script hashes are
recorded in the evidence JSON.

This establishes reproducibility for those inputs on that host. It does not
establish cross-host reproducibility, production signing, an ARMv7 first-install
adapter, Linux 2.6 compatibility or functioning 6300 IQ capture. The ARMv7
runtime still requires its hardware/network profile to be qualified.

## Upgrade storage budget

The native production admission functions were exercised with the actual
authenticated release files and synthetic 1 KiB/4 KiB filesystem blocks,
both shared and separate service-unit filesystems. All eight configurations
accepted their exact byte/inode thresholds and rejected one byte or inode
less: 32 boundary checks. These checks were offline and made no radio changes.

For the ARMv7 release, a conservative same-filesystem, 4 KiB-block calculation
is:

| Component | Bytes |
| --- | ---: |
| Existing terminal installation, including stated allowances | 11,264,000 |
| Native admission for the next upgrade, including its 8 MiB reserve | 28,147,712 |
| Total budget relative to free space before installing AetherPD | **39,411,712** |
| Headroom if the reported free space means 41,000,000 bytes | **1,588,288** |
| Headroom if the report means 41 MiB | **3,579,904** |

The existing-installation calculation includes the selected payload, both
retained release headers and management files, permanent management files,
1 MiB for private state and 64 filesystem blocks for metadata/directories.
The state and metadata amounts are explicit allowances, not measured 6300
usage or enforced caps. Native admission charges the maximum 512 KiB worker,
both staged releases, capsule, management temporaries, journals, receipts,
service definitions and directory growth. At a successful terminal state,
authenticated compaction removes the unused payload and retires the preceding
generation. Rollback data remains available during the transaction.

With service units on the separate root filesystem, the 4 KiB ARMv7 `/user`
budget is 39,321,600 bytes and root admission requires 8,478,720 free bytes.
The root requirement includes its own 8 MiB reserve. Recheck the actual byte
and inode counts on each filesystem immediately before installation.

The model uses equal-sized previous and candidate releases. Admission must
evaluate each actual signed pair, including matching partial state. The
8 MiB reserve is required headroom, not occupied file data. Expanded binaries
belong in RAM; their table size is not an RSS measurement.

## Remaining storage gate

**One additional full release on `/user` exceeds this budget.** Previous and
candidate input descriptors already must reside on tmpfs in the generation
upgrade path. A firmware-retained waveform upload archive or extracted carrier
can still consume persistent space independently of those descriptors.

The final 6000 installation adapter must measure and budget all upload copies,
its first-install capsule and transient files. A small bootstrap followed by
RAM delivery is a candidate design; it has not been implemented or qualified
on the 6300. Do not use this runtime result to approve a full-payload native
waveform installer. First-install high-water use, actual free bytes/inodes,
private-state growth and hardware qualification remain release gates.
