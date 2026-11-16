# Restart-successor runtime rebuild and storage check

Source checkpoint: `2c72ca0a99a452f96fe1880c916250e06bb36cba`.
Status: offline development qualification; no radio changes.

Both complete runtime releases reproduced byte for byte from two separately
copied source directories with different path lengths and separate Go/Zig
caches. All four releases passed the native signature/member verifier. The
run took 164 seconds using Go 1.27.1 and Zig 0.17.1 on one host. Development
signing material remained in RAM/pipes; the checked license bundle was reused.

| Target | Release bytes | Compressed payload bytes | Expanded runtime bytes |
| --- | ---: | ---: | ---: |
| Cortex-A8 ARMv7 | 9,333,427 | 9,011,268 | 22,689,242 |
| ARM64 | 9,007,485 | 8,699,634 | 22,299,778 |

Every ARM64 payload file matches the installed `e0563d1e2` successor,
including its Go shim, DSP and fixed helpers. The new release envelopes use a
fresh development key and source manifest, so they are not the installed
signed release. This check does not qualify the currently unreachable radio's
reboot or imply new RF results.

## Earlier ARMv7 standalone build

The previously recorded standalone ARMv7 shim hash does not match the new
paired build. Its Go build information includes `GOEXPERIMENT=nodwarf5`.
Rebuilding the current source with that flag also did not reproduce the old
binary. No complete immutable input manifest was located for the earlier
standalone build; its discrepancy remains unresolved. Do not substitute that
old binary for the new reproducible artifact or describe the mismatch as
explained by the experiment flag alone. The exact hashes and build information
are retained in the structured report.

## Storage admission

The native installer accepted exact byte/inode thresholds and rejected one
byte or inode less for both architectures, 1 KiB/4 KiB filesystem blocks, and
shared/separate service-unit filesystems: 32 boundary checks. The fixture uses
real current signed release files, equal previous/candidate sizes and synthetic
filesystem capacity. An initial fixture compile omitted its test define and
failed locally; the corrected build and all boundary checks passed.

For ARMv7 with 4 KiB blocks and one filesystem, the calculated upgrade budget
including the native reserve is **39,424,000 bytes**. Against 41,000,000 bytes
free before installation, that leaves **1,576,000 bytes**. With service units
on a separate filesystem, the `/user` budget is 39,333,888 bytes. These are
calculations using explicit state/metadata allowances, not measured 6300 usage.

A full extra release upload still does not fit that margin. The final 6000
adapter must account for firmware-retained uploads, first-install transients,
actual free bytes/inodes and private-state growth. Runtime expansion belongs
in RAM. Actual signed update pairs must pass admission independently.

## Evidence and remaining gates

[Structured report](RESTART-RUNTIME-QUALIFICATION.json) includes both builds,
all payload/member hashes, native budgets and the prior-binary discrepancy.
The [recipe snapshot](evidence/20261116-runtime-restart/run.py) and adjacent
budget fixture/logs preserve the local qualification procedure; paths are the
recorded development workspace, not a shipping installation entry point.

Cross-host builds, production signing/catalogue, actual Linux 2.6/Cortex-A8
execution, 6300 upload/disk high-water measurement and hardware qualification
remain open. The full P1–P10 goal is unchanged.
